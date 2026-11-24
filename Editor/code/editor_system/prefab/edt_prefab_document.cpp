#include "edt_prefab_document.h"
#include "edt_code_commands.h"
#include "edt_file_write_check.h"
#include "edt_prefab_file_edit.h"
#include "edt_override_edit.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/desc/desc_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/common/logger/engine_log.h>
#include <entt/entt.hpp>
#include <glm/gtx/quaternion.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <optional>
#include <utility>

edt::prefab_document::prefab_document(editor_services& services)
	: m_services(services)
{
}

edt::prefab_document::~prefab_document()
{
	unwatch_source();
}

void edt::prefab_document::open(const res::tag& prefab_tag)
{
	if (m_open) {
		close();
	}

	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || !m_services.sfactory() || !m_services.assembler()) {
		egLOG_ERROR("Editor/PrefabCtx", "Cannot open prefab: services not available");
		return;
	}

	// Load prefab resource
	auto prefab_handle = m_services.resources()->require_sync<scn::prefab_desc>(prefab_tag);
	if (!prefab_handle.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to load prefab '{}'", prefab_tag.view());
		return;
	}

	// Load preview template
	auto template_tag = res::tag("edt://templates/prefab_preview.desc");
	auto template_text = m_services.resources()->require_sync<res::text_resource>(template_tag);
	if (!template_text.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to load prefab_preview.desc template");
		return;
	}

	namespace json = boost::json;
	json::object world_json;
	try {
		json::parse_options opts;
		opts.max_depth = 128;
		world_json = json::parse(template_text->str(), {}, opts).as_object();
	} catch (...) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to parse prefab_preview.desc template");
		return;
	}

	// Store template as memory resource for desc_system to parse
	res::tag mem_tag{ "memory://editor/__prefab_preview.desc" };
	m_services.writable_resources()->store(mem_tag, m_services.descs()->serialize_to_bytes(json::value{ world_json }));
	m_services.resources()->signal_changed(mem_tag);

	auto world_res = m_services.resources()->require_sync<scn::world_desc>(mem_tag);
	if (!world_res.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to create world desc for prefab preview");
		return;
	}

	// The preview is a set of its own, so it neither shares the level's time nor its names.
	scn::world_context& preview = ctx_mgr->create_preview(prefab_tag.view(), CONTEXT_NAME, world_res->get_systems());
	m_set_name = scn::context_manager::preview_set_name(prefab_tag.view());
	entt::registry& reg = preview.state();

	// Assemble template (anchor, light, etc.)
	entt::entity root = reg.create();
	m_services.assembler()->spawn_from_desc(reg, root, *world_res, CONTEXT_NAME);

	// Inject prefab camera with separate render targets
	inject_camera(reg);

	// The preview shows the working copy: an edit changes it, never the desc the resource cache holds.
	m_working = std::make_unique<scn::prefab_desc>();
	std::string as_read = take_from(*prefab_handle);
	static std::atomic<std::uint64_t> s_opened{ 0 };
	m_working_tag = std::format("{}/{}", k_working_tag_base, ++s_opened);

	// Load the prefab into the world
	m_root = reg.create();
	// Not under the prefab's own tag: a hot reload of that tag rebuilds an instance from the file and would wipe the edits.
	// The file is watched by the document itself (changed_on_disk).
	scn::assemble_prefab(*m_services.assembler(), reg, m_root, *m_working, m_working->get_root().name, res::tag{ m_working_tag });

	m_source_tag = prefab_tag;
	m_prefab_name = std::string(prefab_tag.pure_name());
	m_open = true;
	// Built again as content of the edited file, as every later sync is: a reference back to it is refused at once.
	sync_preview();
	m_saved_form = std::move(as_read);
	m_dirty = working_form() != m_saved_form;
	m_changed_on_disk = false;
	m_disk_version.clear();
	m_lower_layer = false;
	if (m_files.top_layer) {
		const desc_file_read on_disk = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, prefab_tag);
		m_disk_version = on_disk.version;
		m_lower_layer = on_disk.located && !on_disk.top_layer;
	}

	watch_source();
	egLOG("Editor/PrefabCtx", "Opened prefab '{}' for editing", prefab_tag.view());
}

void edt::prefab_document::close()
{
	if (!m_open) return;

	unwatch_source();
	if (auto ctx_mgr = m_services.ctx_manager.lock()) {
		ctx_mgr->destroy_preview(m_source_tag.view());
	}

	m_open = false;
	m_dirty = false;
	m_changed_on_disk = false;
	m_working.reset();
	m_root = entt::null;
	m_saved_form.clear();
	m_disk_version.clear();
	m_set_name.clear();
	m_source_tag = res::tag::null;
	m_prefab_name.clear();

	egLOG("Editor/PrefabCtx", "Closed prefab editor");
}

// copy_to leaves out what names a file (the tag, the __guid, the __parent): the copy is written back over that file.
std::string edt::prefab_document::take_from(const scn::prefab_desc& source)
{
	++m_replaced;
	source.copy_to(*m_working);
	m_working->set_guid(source.get_guid());
	m_working->set_parent(source.get_parent());
	const scn::ecs_assembler* assembler = m_services.assembler();
	std::string as_read = working_form();
	if (!assembler) return as_read;
	std::size_t moved = 0;
	override_edit::migrate_instance_records(*assembler, m_working->get_root(), &moved);
	// A record that only got the current version is written with it by the next Apply; it does not make the copy edited.
	return moved > 0 ? as_read : working_form();
}

// What the copy holds, without a file read: the "is it edited" test runs on every edit, an Apply builds the bytes.
std::string edt::prefab_document::working_form() const
{
	if (!m_working) {
		return {};
	}
	namespace json = boost::json;
	json::object body;
	m_working->serialize(body);
	body["__type"] = std::string(scn::prefab_desc::__type);
	body["__guid"] = m_working->get_guid();
	body["__parent"] = std::string(m_working->get_parent().view());
	return json::serialize(body);
}

std::vector<std::byte> edt::prefab_document::working_bytes() const
{
	return m_working ? prefab_file_bytes(m_services, *m_working, m_source_tag) : std::vector<std::byte>{};
}

edt::autosave_snapshot edt::prefab_document::autosave_capture() const
{
	autosave_snapshot out;
	if (!m_open || !m_working) {
		out.error = "the prefab is not open";
		return out;
	}
	std::string why;
	out.bytes = prefab_file_bytes(m_services, *m_working, m_source_tag, &why);
	if (out.bytes.empty()) {
		out.error = why.empty() ? "the prefab has no file form" : why;
	}
	return out;
}

void edt::prefab_document::sync_preview()
{
	entt::registry* reg = get_registry();
	scn::ecs_assembler* assembler = m_services.assembler();
	if (reg && assembler && m_working && m_root != entt::null && reg->valid(m_root)) {
		// The working copy is the file's content: its records may instantiate a prefab its nodes instantiate (#993).
		scn::reconcile(*assembler, *reg, m_root, m_working->get_root(), scn::reconcile_mode::reload, m_source_tag);
		// reconcile moves the children only: the root's own transform is placed here, whether or not the file spells it out.
		reg->emplace_or_replace<scn::local_transform>(m_root, m_working->get_root().get_local_transform());
		if (reg->ctx().contains<ecs::event<scn::transform_updated>>()) {
			reg->ctx().get<ecs::event<scn::transform_updated>>().emit(m_root);
		}
	}
}

void edt::prefab_document::announce()
{
	if (m_events) {
		m_events->document_changed.emit(id(), m_generation);
	}
}

bool edt::prefab_document::edit(const std::function<bool(scn::prefab_desc&)>& change)
{
	if (!m_open || !m_working || edits_locked()) {
		return false;
	}
	if (!change(*m_working)) {
		return false;
	}
	++m_generation;
	sync_preview();
	m_dirty = working_form() != m_saved_form;
	announce();
	return true;
}

std::string edt::prefab_document::apply_blocker() const
{
	if (!m_open) return "the prefab is not open";
	if (edits_locked()) return "Stop Play to apply the prefab";
	if (!m_files.top_layer) return "the project files are not known";
	if (m_lower_layer) return "this prefab belongs to a lower layer (the engine or a package): it cannot be written from the project";
	if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) return *why;
	return {};
}

scn::prefab_desc::prefab_node* edt::prefab_document::working_node(const std::vector<std::uint64_t>& path)
{
	if (!m_open || !m_working || edits_locked()) {
		return nullptr;
	}
	scn::prefab_desc::prefab_node* node = &m_working->get_root();
	for (const std::uint64_t id : path) {
		const auto found = std::ranges::find_if(node->children, [id](const scn::prefab_desc::prefab_node& child) { return child.node_id == id; });
		if (found == node->children.end()) {
			return nullptr;
		}
		node = &*found;
	}
	return node;
}

bool edt::prefab_document::notify_edited()
{
	if (!m_open || !m_working || edits_locked()) {
		return false;
	}
	++m_generation;
	sync_preview();
	m_dirty = working_form() != m_saved_form;
	announce();
	return true;
}

edt::save_result edt::prefab_document::save()
{
	return save_with(false);
}

edt::save_result edt::prefab_document::save_without_base()
{
	return save_with(true);
}

edt::save_result edt::prefab_document::save_with(bool without_base)
{
	m_layer_refused = false;
	if (!m_open || !m_working) {
		return save_result::failure("the prefab is not open");
	}
	if (edits_locked()) {
		return save_result::failure("Stop Play to apply the prefab");
	}
	if (!m_dirty) {
		return save_result::ok();
	}
	if (!m_files.top_layer) {
		return save_result::failure("the project files are not known: nothing was written");
	}
	if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) {
		return save_result::failure(*why);
	}
	desc::desc_system* descs = m_services.descs();
	res::resource_system* reader = m_services.resources();
	res::resource_system* writer = m_services.writable_resources();
	if (!descs || !reader || !writer) {
		return save_result::failure("the project resources are not available");
	}
	const file_write_check allowed = check_file_write(*reader, *descs, m_files, m_source_tag, m_disk_version);
	if (allowed.changed_on_disk && !m_changed_on_disk) {
		// The watcher may not have said so yet: the file is known to differ now, and Save All's question depends on the flag.
		m_changed_on_disk = true;
		announce();
	}
	if (!allowed.error.empty()) {
		return save_result::failure(allowed.error);
	}
	std::string why;
	std::vector<std::byte> bytes;
	if (without_base) {
		scn::prefab_desc whole;
		m_working->copy_to(whole);
		whole.set_guid(m_working->get_guid());
		bytes = prefab_file_bytes(m_services, whole, m_source_tag, &why);
	} else {
		bytes = prefab_file_bytes(m_services, *m_working, m_source_tag, &why);
	}
	m_layer_refused = !without_base && bytes.empty() && m_working->get_parent().is_valid();
	if (bytes.empty()) {
		return save_result::failure(std::format("{} cannot be written over its base: {}. Nothing was written, the edits stay.", m_source_tag.view(), why.empty() ? "no file form" : why));
	}
	if (bytes.size() > k_desc_file_limit) {
		return save_result::failure(std::format("{} would be {} bytes: a prefab holds at most {}", m_source_tag.view(), bytes.size(), k_desc_file_limit));
	}
	if (!writer->store_sync(m_source_tag, bytes)) {
		return save_result::failure(std::format("{} could not be written", m_source_tag.view()));
	}
	writer->signal_changed(m_source_tag);
	if (without_base) m_working->set_parent({});
	m_saved_form = working_form();
	m_disk_version = code_version_of(bytes);
	m_dirty = false;
	m_changed_on_disk = false;
	announce();
	return save_result::ok();
}

edt::save_result edt::prefab_document::overwrite()
{
	if (!m_open || !m_working) {
		return save_result::failure("the prefab is not open");
	}
	// Nothing to write: the file as it is now is what the document takes.
	if (!m_dirty) {
		const std::string error = reload();
		return error.empty() ? save_result::ok() : save_result::failure(error);
	}
	desc::desc_system* descs = m_services.descs();
	res::resource_system* reader = m_services.resources();
	if (!descs || !reader || !m_files.top_layer) {
		return save_result::failure("the project files are not known: nothing was written");
	}
	const desc_file_read on_disk = read_desc_file(*reader, *descs, m_files, m_source_tag);
	if (!on_disk.error.empty() && !on_disk.located) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", m_source_tag.view(), on_disk.error));
	}
	const std::string previous_version = std::exchange(m_disk_version, on_disk.version);
	const save_result saved = save();
	if (!saved.saved) {
		m_disk_version = previous_version;
		m_changed_on_disk = true;
	}
	return saved;
}

void edt::prefab_document::watch_source()
{
	unwatch_source();
	m_watching = m_services.writable_resources();
	if (m_watching) {
		m_watching->watch(m_source_tag, this, [this](const res::tag& changed) { on_source_changed(changed); });
	}
}

void edt::prefab_document::unwatch_source()
{
	if (m_watching) {
		m_watching->unwatch(m_source_tag, this);
		m_watching = nullptr;
	}
}

void edt::prefab_document::on_source_changed(const res::tag& changed)
{
	if (!m_open || changed != m_source_tag || !m_files.top_layer) {
		return;
	}
	desc::desc_system* descs = m_services.descs();
	res::resource_system* reader = m_services.resources();
	if (!descs || !reader) {
		return;
	}
	const desc_file_read on_disk = read_desc_file(*reader, *descs, m_files, m_source_tag);
	if (on_disk.version == m_disk_version) {
		if (m_changed_on_disk) {
			m_changed_on_disk = false;
			announce();
		}
		return;
	}
	if (!m_dirty && !edits_locked()) {
		if (const std::string error = revert(); error.empty()) {
			return;
		} else {
			egLOG_WARN("editor", "{}", error);
		}
	}
	if (!m_changed_on_disk) {
		m_changed_on_disk = true;
		announce();
	}
}

std::string edt::prefab_document::revert()
{
	if (!m_open || !m_working) {
		return "the prefab is not open";
	}
	if (edits_locked()) {
		return "Stop Play to revert the prefab";
	}
	desc::desc_system* descs = m_services.descs();
	res::resource_system* reader = m_services.resources();
	res::resource_system* writer = m_services.writable_resources();
	if (!descs || !reader || !writer) {
		return "the project resources are not available";
	}
	std::string version;
	if (m_files.top_layer) {
		const desc_file_read on_disk = read_desc_file(*reader, *descs, m_files, m_source_tag);
		if (!on_disk.error.empty()) {
			return std::format("{} cannot be read: {}", m_source_tag.view(), on_disk.error);
		}
		version = on_disk.version;
	}
	// The cache may hold an older read of the file: drop it, then take the prefab fresh.
	// Nothing written: only a file that differs from what the open instances read needs the others told.
	if (!version.empty() && version != m_disk_version) {
		writer->signal_changed(m_source_tag);
	} else {
		writer->evict_cached(m_source_tag);
	}
	auto handle = reader->require_sync<scn::prefab_desc>(m_source_tag);
	if (!handle.is_ready()) {
		return std::format("{} could not be loaded", m_source_tag.view());
	}
	m_saved_form = take_from(*handle);
	++m_generation;
	sync_preview();
	m_disk_version = std::move(version);
	m_dirty = working_form() != m_saved_form;
	m_changed_on_disk = false;
	announce();
	return {};
}

entt::registry* edt::prefab_document::get_registry()
{
	if (!m_open) return nullptr;

	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return nullptr;

	scn::world_context* preview = ctx_mgr->find_preview(m_source_tag.view());
	return preview ? &preview->state() : nullptr;
}

void edt::prefab_document::inject_camera(entt::registry& reg)
{
	entt::entity cam = reg.create();
	reg.emplace<edt::prefab_camera_tag>(cam);

	scn::camera_component cam_comp;
	cam_comp.fov           = m_camera_state.fov;
	cam_comp.near_distance = m_camera_state.near_distance;
	cam_comp.far_distance  = m_camera_state.far_distance;
	cam_comp.color_targets.push_back(res::tag{ RT_COLOR });
	cam_comp.depth_target      = res::tag{ RT_DEPTH };
	cam_comp.tp_accum_target   = res::tag{ RT_TP_ACCUM };
	cam_comp.tp_reveal_target  = res::tag{ RT_TP_REVEAL };
	reg.emplace<scn::camera_component>(cam, cam_comp);

	scn::mouse_controller_component ctrl;
	ctrl.position       = m_camera_state.position;
	ctrl.rotation       = m_camera_state.rotation;
	ctrl.distance       = m_camera_state.distance;
	ctrl.movement_speed = m_camera_state.movement_speed;
	ctrl.rotating_speed = m_camera_state.rotating_speed;
	reg.emplace<scn::mouse_controller_component>(cam, ctrl);

	glm::mat4 orientation = glm::toMat4(glm::quat(ctrl.rotation));
	glm::mat4 cam_matrix = glm::translate(glm::mat4(1.0f), ctrl.position)
		* orientation
		* glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, ctrl.distance));
	reg.emplace<scn::local_transform>(cam, scn::local_transform{ cam_matrix });
	// The camera has no parent and nothing emits transform_updated for it until it moves, so the
	// transform system would leave an identity world matrix (and the render camera at the origin).
	reg.emplace<scn::world_transform>(cam, scn::world_transform{ cam_matrix });
	reg.emplace<scn::name_component>(cam, "Prefab Camera");
	reg.emplace<scn::depth_level>(cam);
}
