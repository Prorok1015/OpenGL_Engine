#include "edt_prefab_document.h"
#include "edt_code_commands.h"
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
#include <format>
#include <optional>

edt::prefab_document::prefab_document(editor_services& services)
	: m_services(services)
{
}

edt::prefab_document::~prefab_document() = default;

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
	prefab_handle->copy_to(*m_working);

	// Load the prefab into the world
	m_root = reg.create();
	scn::assemble_prefab(*m_services.assembler(), reg, m_root, *m_working, m_working->get_root().name, prefab_tag);

	m_source_tag = prefab_tag;
	m_prefab_name = std::string(prefab_tag.pure_name());
	m_open = true;
	m_dirty = false;
	m_saved_bytes = working_bytes();
	m_disk_version.clear();
	if (m_files.top_layer) {
		m_disk_version = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, prefab_tag).version;
	}

	egLOG("Editor/PrefabCtx", "Opened prefab '{}' for editing", prefab_tag.view());
}

void edt::prefab_document::close()
{
	if (!m_open) return;

	if (auto ctx_mgr = m_services.ctx_manager.lock()) {
		ctx_mgr->destroy_preview(m_source_tag.view());
	}

	m_open = false;
	m_dirty = false;
	m_working.reset();
	m_root = entt::null;
	m_saved_bytes.clear();
	m_disk_version.clear();
	m_set_name.clear();
	m_source_tag = res::tag::null;
	m_prefab_name.clear();

	egLOG("Editor/PrefabCtx", "Closed prefab editor");
}

std::vector<std::byte> edt::prefab_document::working_bytes() const
{
	desc::desc_system* descs = m_services.descs();
	if (!m_working || !descs) {
		return {};
	}
	namespace json = boost::json;
	json::object body;
	m_working->serialize(body);
	descs->reduce_to_parent(*m_working, body);
	body["__type"] = std::string(scn::prefab_desc::__type);
	// A __guid the file had stays until the project's migration takes it out; none is made up (the adapter does the same).
	if (!m_working->get_guid().empty()) {
		body["__guid"] = m_working->get_guid();
	}
	descs->stamp_tree(body);
	json::value file{ std::move(body) };
	descs->write_references_over(file, m_source_tag);
	return descs->serialize_to_bytes(file);
}

void edt::prefab_document::sync_preview()
{
	entt::registry* reg = get_registry();
	scn::ecs_assembler* assembler = m_services.assembler();
	if (reg && assembler && m_working && m_root != entt::null && reg->valid(m_root)) {
		scn::reconcile(*assembler, *reg, m_root, m_working->get_root(), scn::reconcile_mode::reload);
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
	m_dirty = working_bytes() != m_saved_bytes;
	announce();
	return true;
}

edt::save_result edt::prefab_document::save()
{
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
	const desc_file_read on_disk = read_desc_file(*reader, *descs, m_files, m_source_tag);
	if (!on_disk.error.empty() && !on_disk.located) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", m_source_tag.view(), on_disk.error));
	}
	if (!on_disk.top_layer) {
		return save_result::failure(std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/: nothing was written", m_source_tag.view()));
	}
	if (on_disk.in_open_level) {
		return save_result::failure(std::format("{} belongs to the open level now: nothing was written", m_source_tag.view()));
	}
	if (on_disk.version != m_disk_version) {
		return save_result::failure(std::format("{} changed on disk since it was opened: nothing was written", m_source_tag.view()));
	}
	const std::vector<std::byte> bytes = working_bytes();
	if (bytes.size() > k_desc_file_limit) {
		return save_result::failure(std::format("{} would be {} bytes: a prefab holds at most {}", m_source_tag.view(), bytes.size(), k_desc_file_limit));
	}
	if (!writer->store_sync(m_source_tag, bytes)) {
		return save_result::failure(std::format("{} could not be written", m_source_tag.view()));
	}
	writer->signal_changed(m_source_tag);
	m_saved_bytes = bytes;
	m_disk_version = code_version_of(bytes);
	m_dirty = false;
	announce();
	return save_result::ok();
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
	writer->signal_changed(m_source_tag);
	auto handle = reader->require_sync<scn::prefab_desc>(m_source_tag);
	if (!handle.is_ready()) {
		return std::format("{} could not be loaded", m_source_tag.view());
	}
	handle->copy_to(*m_working);
	++m_generation;
	sync_preview();
	m_saved_bytes = working_bytes();
	m_disk_version = std::move(version);
	m_dirty = false;
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
