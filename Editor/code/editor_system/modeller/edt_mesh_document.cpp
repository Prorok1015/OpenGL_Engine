#include "edt_mesh_document.h"
#include "edt_code_commands.h"
#include "edt_file_write_check.h"
#include "edt_editor_camera.h"
#include "edt_mesh_shape_asset.h"
#include "edt_prefab_document.h"
#include "edt_shape_menu_model.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <snk/common/eng_transform_3d.hpp>
#include <glm/gtx/quaternion.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <vector>
#include <format>
#include <utility>

namespace json = boost::json;

namespace
{
	std::string_view without_suffix(std::string_view text, std::string_view suffix)
	{
		return text.substr(0, text.size() - suffix.size());
	}

	std::vector<std::byte> bytes_of(std::string_view text)
	{
		const auto* first = reinterpret_cast<const std::byte*>(text.data());
		return std::vector<std::byte>(first, first + text.size());
	}

	// The preview takes the geometry as the arrays form of its desc: a memory:// file has no binary file beside it.
	std::vector<std::byte> preview_bytes(const rnd::geometry_desc& geometry)
	{
		json::object body;
		geometry.serialize(body);
		body["__type"] = "geometry_desc";
		return bytes_of(json::serialize(body));
	}
}

edt::mesh_document::mesh_document(editor_services& services)
	: m_services(services)
{
}

edt::mesh_document::~mesh_document()
{
	close();
}

void edt::mesh_document::open(const res::tag& source)
{
	if (m_open) {
		close();
	}
	m_error.clear();

	const std::string_view text = source.view();
	std::string poly_text;
	if (text.ends_with(mesh::geometry_file_suffix)) {
		poly_text = std::string(without_suffix(text, mesh::geometry_file_suffix)) + std::string(mesh::poly_file_suffix);
	} else if (text.ends_with(mesh::poly_file_suffix)) {
		poly_text = std::string(text);
	} else {
		m_error = std::format("{} is neither a {} nor a {} file", text, mesh::poly_file_suffix, mesh::geometry_file_suffix);
		return;
	}
	const std::string_view stem = without_suffix(poly_text, mesh::poly_file_suffix);
	m_poly_tag = res::tag(poly_text);
	m_geometry_tag = res::tag(std::string(stem) + std::string(mesh::geometry_file_suffix));

	if (!m_services.resources() || !m_services.writable_resources() || !m_services.descs() || !m_files.top_layer) {
		m_error = "the project files are not known";
		return;
	}
	if (!load_source(m_error)) {
		if (text.ends_with(mesh::geometry_file_suffix) && m_error.starts_with("missing:")) {
			m_error = std::format("{} has no source to edit ({} is missing): it was not made by the editor's shape tools", text, m_poly_tag.view());
		}
		return;
	}

	static std::atomic<std::uint64_t> s_opened{ 0 };
	m_serial = ++s_opened;
	m_preview_tag = res::tag(std::format("memory://editor/mesh/{}.geom.desc", m_serial));
	m_title = std::string(stem.substr(stem.rfind('/') == std::string_view::npos ? 0 : stem.rfind('/') + 1));
	m_geometry_stale = false;
	m_stale_message.clear();
	m_open = true;
	rebake();
	open_preview_world();
	if (!m_has_world) release_preview_resources();
	watch_source();
	egLOG("Editor/Mesh", "Opened mesh '{}' for editing", m_poly_tag.view());
}

bool edt::mesh_document::load_source(std::string& error)
{
	const desc_file_read file = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_poly_tag);
	if (file.missing) {
		error = "missing: " + std::string(m_poly_tag.view());
		return false;
	}
	if (!file.error.empty()) {
		error = std::format("{} cannot be read: {}", m_poly_tag.view(), file.error);
		return false;
	}
	mesh::poly_mesh_desc loaded;
	try {
		json::parse_options options;
		options.max_depth = 128;
		const json::value parsed = json::parse(std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()), {}, options);
		if (!parsed.is_object()) {
			error = std::format("{} is not a JSON object", m_poly_tag.view());
			return false;
		}
		mesh::poly_mesh_from_json(parsed.as_object(), loaded);
	} catch (const std::exception& failure) {
		error = std::format("{} is not a mesh: {}", m_poly_tag.view(), failure.what());
		return false;
	}
	m_working = std::move(loaded);
	m_view.reset();
	m_saved_text = working_text();
	m_disk_version = file.version;
	m_dirty = false;
	m_changed_on_disk = false;
	check_geometry_match();
	return true;
}

void edt::mesh_document::check_geometry_match()
{
	m_geometry_mismatch = false;
	m_mismatch_message.clear();
	const desc_file_read geometry = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_geometry_tag);
	if (geometry.missing || !geometry.error.empty()) return;
	const std::string existing(reinterpret_cast<const char*>(geometry.bytes.data()), geometry.bytes.size());
	std::string why;
	switch (mesh::geometry_is_bake_of(m_saved_text, existing, why)) {
	case mesh::geometry_match::same:
		return;
	case mesh::geometry_match::differs:
		m_mismatch_message = std::format("{} is not the bake of {} (the source changed after the geometry was written, or the file is another mesh's). The game draws the geometry file. Overwrite geometry writes it from the source.",
			m_geometry_tag.view(), m_poly_tag.view());
		break;
	case mesh::geometry_match::unchecked:
		m_mismatch_message = std::format("{} cannot be compared with {}: {}", m_geometry_tag.view(), m_poly_tag.view(), why);
		break;
	}
	m_geometry_mismatch = true;
}

std::string edt::mesh_document::working_text() const
{
	return mesh::poly_file_text(m_working);
}

edt::autosave_snapshot edt::mesh_document::autosave_capture() const
{
	autosave_snapshot out;
	if (!m_open) {
		out.error = "the mesh is not open";
		return out;
	}
	out.bytes = bytes_of(working_text());
	return out;
}

void edt::mesh_document::close()
{
	if (!m_open) return;
	unwatch_source();
	close_preview_world();
	if (res::resource_system* writer = m_services.writable_resources()) {
		writer->erase(m_preview_tag);
	}
	m_open = false;
	m_dirty = false;
	m_geometry_stale = false;
	m_geometry_mismatch = false;
	m_mismatch_message.clear();
	m_changed_on_disk = false;
	m_stale_message.clear();
	m_working = {};
	m_view.reset();
	m_baked = {};
	m_stats = {};
}

void edt::mesh_document::rebake()
{
	m_baked = mesh::bake(m_working.mesh);
	m_stats = {};
	m_stats.vertices = m_working.mesh.vertices().size();
	m_stats.faces = m_working.mesh.faces().size();
	m_stats.triangles = m_baked.geometry.indices.size() / 3;
	m_stats.skipped_faces = m_baked.skipped_faces.size();
	m_preview_problem.clear();

	res::resource_system* writer = m_services.writable_resources();
	if (!writer) {
		m_preview_problem = "the project resources are not available";
		return;
	}
	if (m_baked.geometry.indices.empty()) {
		m_preview_problem = "the mesh has no triangle to draw";
		writer->erase(m_preview_tag);
		writer->signal_changed(m_preview_tag);
		sync_preview_node();
		return;
	}
	try {
		writer->store(m_preview_tag, preview_bytes(m_baked.geometry));
	} catch (const std::exception& failure) {
		m_preview_problem = std::format("the mesh cannot be previewed: {}", failure.what());
		return;
	}
	writer->signal_changed(m_preview_tag);
	sync_preview_node();
}

void edt::mesh_document::announce()
{
	if (m_events) {
		m_events->document_changed.emit(id(), m_generation);
	}
}

bool edt::mesh_document::edit(const std::function<bool(mesh::poly_mesh&)>& change)
{
	if (!m_open || edits_locked()) {
		return false;
	}
	if (!change(m_working.mesh)) {
		return false;
	}
	++m_generation;
	rebake();
	m_dirty = working_text() != m_saved_text;
	announce();
	return true;
}

std::string edt::mesh_document::geometry_file_problem(bool replace_foreign_geometry) const
{
	const desc_file_read geometry = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_geometry_tag);
	if (geometry.in_open_level) {
		return std::format("{} belongs to the open level now: nothing was written", m_geometry_tag.view());
	}
	if (!geometry.missing && !geometry.error.empty()) {
		return std::format("{} cannot be checked before it is written: {}", m_geometry_tag.view(), geometry.error);
	}
	if (!geometry.missing && !geometry.top_layer) {
		return std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/: nothing was written", m_geometry_tag.view());
	}
	// Only while the source is about to change, and not after a failed pair: that file is ours and older than the source on purpose.
	if (!replace_foreign_geometry && m_dirty && !m_geometry_stale && !geometry.missing) {
		const std::string existing(reinterpret_cast<const char*>(geometry.bytes.data()), geometry.bytes.size());
		std::string why;
		switch (mesh::geometry_is_bake_of(m_saved_text, existing, why)) {
		case mesh::geometry_match::same:
			break;
		case mesh::geometry_match::differs:
			return std::format("{} exists and was not made from {} (an imported mesh or another source): nothing was written. Rename one of the files, or use Overwrite geometry to replace it.",
				m_geometry_tag.view(), m_poly_tag.view());
		case mesh::geometry_match::unchecked:
			return std::format("{} cannot be compared with {}: {}. Nothing was written.", m_geometry_tag.view(), m_poly_tag.view(), why);
		}
	}
	return {};
}

edt::save_result edt::mesh_document::save()
{
	return save_impl(false);
}

edt::save_result edt::mesh_document::overwrite_geometry()
{
	return save_impl(true);
}

edt::save_result edt::mesh_document::save_impl(bool replace_foreign_geometry)
{
	if (!m_open) {
		return save_result::failure("the mesh is not open");
	}
	if (edits_locked()) {
		return save_result::failure("Stop Play to save the mesh");
	}
	if (!is_dirty() && !(replace_foreign_geometry && m_geometry_mismatch)) {
		return save_result::ok();
	}
	if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) {
		return save_result::failure(*why);
	}
	if (!m_preview_problem.empty()) {
		return save_result::failure(std::format("{}: nothing was written", m_preview_problem));
	}
	res::resource_system* reader = m_services.resources();
	res::resource_system* writer = m_services.writable_resources();
	desc::desc_system* descs = m_services.descs();
	if (!reader || !writer || !descs) {
		return save_result::failure("the project resources are not available");
	}

	const std::string geometry_problem = geometry_file_problem(replace_foreign_geometry);
	if (!geometry_problem.empty()) {
		return save_result::failure(geometry_problem);
	}
	bool source_written = false;
	if (m_dirty) {
		const file_write_check allowed = check_file_write(*reader, *descs, m_files, m_poly_tag, m_disk_version);
		if (allowed.changed_on_disk && !m_changed_on_disk) {
			m_changed_on_disk = true;
			announce();
		}
		if (!allowed.error.empty()) {
			return save_result::failure(allowed.error);
		}
		const std::vector<std::byte> bytes = bytes_of(working_text());
		if (!writer->store_sync(m_poly_tag, bytes)) {
			return save_result::failure(std::format("{} could not be written", m_poly_tag.view()));
		}
		writer->signal_changed(m_poly_tag);
		m_saved_text = working_text();
		m_disk_version = code_version_of(bytes);
		m_dirty = false;
		m_changed_on_disk = false;
		m_geometry_stale = true;
		source_written = true;
	}

	const std::string written = m_write_geometry ? m_write_geometry(*writer, m_geometry_tag, m_baked.geometry)
		: mesh::write_mesh_geometry(*writer, m_geometry_tag, m_baked.geometry);
	if (!written.empty() && !source_written && !m_geometry_stale) {
		return save_result::failure(std::format("{} could not be written: {}", m_geometry_tag.view(), written));
	}
	if (!written.empty()) {
		m_stale_message = std::format("{} was saved, but {} could not be written ({}). The game still loads the old geometry: press Save again.",
			m_poly_tag.view(), m_geometry_tag.view(), written);
		announce();
		return save_result::failure(m_stale_message);
	}
	m_geometry_stale = false;
	m_geometry_mismatch = false;
	m_mismatch_message.clear();
	m_stale_message.clear();
	announce();
	return save_result::ok();
}

std::string edt::mesh_document::revert()
{
	if (!m_open) {
		return "the mesh is not open";
	}
	if (edits_locked()) {
		return "Stop Play to revert the mesh";
	}
	std::string error;
	if (!load_source(error)) {
		return error;
	}
	++m_generation;
	rebake();
	announce();
	return {};
}

edt::save_result edt::mesh_document::overwrite()
{
	if (!m_open) {
		return save_result::failure("the mesh is not open");
	}
	if (!m_dirty) {
		const std::string error = revert();
		return error.empty() ? save_result::ok() : save_result::failure(error);
	}
	if (!m_services.resources() || !m_services.descs() || !m_files.top_layer) {
		return save_result::failure("the project files are not known: nothing was written");
	}
	const desc_file_read on_disk = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_poly_tag);
	if (!on_disk.error.empty() && !on_disk.located) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", m_poly_tag.view(), on_disk.error));
	}
	const std::string previous_version = std::exchange(m_disk_version, on_disk.version);
	const save_result saved = save();
	// Still dirty means the source was not written (a written source makes it clean even when the geometry pair failed).
	if (!saved.saved && m_dirty) {
		m_disk_version = previous_version;
		m_changed_on_disk = true;
	}
	return saved;
}

void edt::mesh_document::watch_source()
{
	unwatch_source();
	m_watching = m_services.writable_resources();
	if (m_watching) {
		m_watching->watch(m_poly_tag, this, [this](const res::tag& changed) { on_source_changed(changed); });
	}
}

void edt::mesh_document::unwatch_source()
{
	if (m_watching) {
		m_watching->unwatch(m_poly_tag, this);
		m_watching = nullptr;
	}
}

void edt::mesh_document::on_source_changed(const res::tag& changed)
{
	if (!m_open || changed != m_poly_tag) {
		return;
	}
	const desc_file_read on_disk = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_poly_tag);
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
			egLOG_WARN("Editor/Mesh", "{}", error);
		}
	}
	if (!m_changed_on_disk) {
		m_changed_on_disk = true;
		announce();
	}
}

namespace
{
	const res::tag k_template_tag{ "edt://templates/prefab_preview.desc" };

	float frame_distance(const edt::mesh::poly_mesh& mesh)
	{
		float radius = 0.f;
		for (const auto& entry : mesh.vertices()) radius = std::max(radius, glm::length(entry.second.position));
		return std::max(radius * 2.5f, 1.f);
	}
}

entt::registry* edt::mesh_document::registry()
{
	if (!m_open || !m_has_world) return nullptr;
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return nullptr;
	scn::world_context* preview = ctx_mgr->find_preview(m_poly_tag.view());
	return preview ? &preview->state() : nullptr;
}

entt::entity edt::mesh_document::camera_entity()
{
	entt::registry* reg = registry();
	if (!reg) return entt::null;
	for (const entt::entity entity : reg->view<scn::camera_component>()) {
		const scn::camera_component& found = reg->get<scn::camera_component>(entity);
		if (!found.color_targets.empty() && found.color_targets.front().view() == m_rt_color) return entity;
	}
	return entt::null;
}

scn::camera_component* edt::mesh_document::camera()
{
	const entt::entity entity = camera_entity();
	return entity == entt::null ? nullptr : &registry()->get<scn::camera_component>(entity);
}

scn::mouse_controller_component* edt::mesh_document::camera_controller()
{
	const entt::entity entity = camera_entity();
	return entity == entt::null ? nullptr : registry()->try_get<scn::mouse_controller_component>(entity);
}

std::optional<glm::mat4> edt::mesh_document::view_projection()
{
	const entt::entity entity = camera_entity();
	if (entity == entt::null) return std::nullopt;
	entt::registry& reg = *registry();
	const scn::camera_component& cam = reg.get<scn::camera_component>(entity);
	if (cam.m_viewport.size.x < 1 || cam.m_viewport.size.y < 1) return std::nullopt;
	glm::mat4 view{ 1.f };
	if (const auto* transform = reg.try_get<scn::world_transform>(entity)) view = glm::inverse(transform->world);
	const glm::ivec4 viewport = cam.m_viewport;
	return scn::camera_projection_matrix(cam, viewport) * view;
}

void edt::mesh_document::park_camera()
{
	if (scn::camera_component* shown = camera()) {
		shown->m_viewport.size = decltype(shown->m_viewport.size){};
	}
}

void edt::mesh_document::open_preview_world()
{
	m_has_world = false;
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || !m_services.sfactory() || !m_services.assembler()) return;
	if (!m_services.resources() || !m_services.writable_resources() || !m_services.descs()) return;

	const auto template_text = m_services.resources()->require_sync<res::text_resource>(k_template_tag);
	if (!template_text.is_ready()) {
		egLOG_ERROR("Editor/Mesh", "Cannot show the mesh: the preview template could not be loaded");
		return;
	}
	json::object world_json;
	try {
		json::parse_options options;
		options.max_depth = 128;
		world_json = json::parse(template_text->str(), {}, options).as_object();
	} catch (const std::exception&) {
		egLOG_ERROR("Editor/Mesh", "Cannot show the mesh: the preview template is not JSON");
		return;
	}
	const res::tag world_tag{ std::format("memory://editor/__mesh_preview/{}.desc", m_serial) };
	m_world_tag = world_tag;
	m_services.writable_resources()->store(world_tag, m_services.descs()->serialize_to_bytes(json::value{ world_json }));
	m_services.resources()->signal_changed(world_tag);
	const auto world_res = m_services.resources()->require_sync<scn::world_desc>(world_tag);
	if (!world_res.is_ready()) {
		egLOG_ERROR("Editor/Mesh", "Cannot show the mesh: the preview world could not be made");
		return;
	}

	scn::world_context& preview = ctx_mgr->create_preview(m_poly_tag.view(), context_name(), world_res->get_systems());
	m_set_name = scn::context_manager::preview_set_name(m_poly_tag.view());
	entt::registry& reg = preview.state();
	const entt::entity template_root = reg.create();
	m_services.assembler()->spawn_from_desc(reg, template_root, *world_res, context_name());

	const std::uint64_t suffix = m_serial;
	m_rt_color = std::format("memory://__mesh_camera_rt/{}", suffix);
	m_rt_depth = std::format("memory://__mesh_depth_rt/{}", suffix);
	m_rt_accum = std::format("memory://__mesh_tp_accum_rt/{}", suffix);
	m_rt_reveal = std::format("memory://__mesh_tp_reveal_rt/{}", suffix);

	editor_camera_state view;
	view.position = glm::vec3{ 0.f };
	view.distance = frame_distance(m_working.mesh);
	const entt::entity cam = reg.create();
	reg.emplace<prefab_camera_tag>(cam);
	scn::camera_component cam_comp;
	cam_comp.fov = view.fov;
	cam_comp.near_distance = view.near_distance;
	cam_comp.far_distance = view.far_distance;
	cam_comp.color_targets.push_back(res::tag{ m_rt_color });
	cam_comp.depth_target = res::tag{ m_rt_depth };
	cam_comp.tp_accum_target = res::tag{ m_rt_accum };
	cam_comp.tp_reveal_target = res::tag{ m_rt_reveal };
	reg.emplace<scn::camera_component>(cam, cam_comp);
	scn::mouse_controller_component ctrl;
	ctrl.position = view.position;
	ctrl.rotation = view.rotation;
	ctrl.distance = view.distance;
	ctrl.movement_speed = view.movement_speed;
	ctrl.rotating_speed = view.rotating_speed;
	reg.emplace<scn::mouse_controller_component>(cam, ctrl);
	const glm::mat4 cam_matrix = glm::translate(glm::mat4(1.0f), ctrl.position) * glm::toMat4(glm::quat(ctrl.rotation))
		* glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, ctrl.distance));
	reg.emplace<scn::local_transform>(cam, scn::local_transform{ cam_matrix });
	reg.emplace<scn::world_transform>(cam, scn::world_transform{ cam_matrix });
	reg.emplace<scn::name_component>(cam, "Mesh Camera");
	reg.emplace<scn::depth_level>(cam);

	static std::atomic<std::uint64_t> s_worlds{ 0 };
	m_working_tag = std::format("memory://editor/__mesh_working/{}", ++s_worlds);
	m_preview_prefab = std::make_unique<scn::prefab_desc>();
	m_preview_prefab->get_root().name = "mesh";
	m_preview_prefab->get_root().children.push_back(mesh::mesh_node("mesh", mesh::mesh_fields(m_preview_tag.view(), {})));
	m_root = reg.create();
	scn::assemble_prefab(*m_services.assembler(), reg, m_root, *m_preview_prefab, m_preview_prefab->get_root().name, res::tag{ m_working_tag });
	m_has_world = true;
	sync_preview_node();
}

void edt::mesh_document::release_preview_world()
{
	if (m_has_world) {
		if (auto ctx_mgr = m_services.ctx_manager.lock()) ctx_mgr->destroy_preview(m_poly_tag.view());
	}
	m_has_world = false;
	m_root = entt::null;
	m_preview_prefab.reset();
	m_set_name.clear();
}

void edt::mesh_document::rebuild_preview_world()
{
	if (!m_open || m_has_world) return;
	open_preview_world();
	if (!m_has_world) release_preview_resources();
	rebake();
}

void edt::mesh_document::close_preview_world()
{
	release_preview_world();
	release_preview_resources();
}

void edt::mesh_document::release_preview_resources()
{
	if (m_world_tag.is_valid()) {
		if (res::resource_system* writer = m_services.writable_resources()) writer->erase(m_world_tag);
		m_world_tag = res::tag{};
	}
	if (!m_rt_color.empty()) {
		if (m_release_targets) {
			m_release_targets({ res::tag{ m_rt_color }, res::tag{ m_rt_depth }, res::tag{ m_rt_accum }, res::tag{ m_rt_reveal } });
		}
		m_rt_color.clear();
		m_rt_depth.clear();
		m_rt_accum.clear();
		m_rt_reveal.clear();
	}
}

std::string edt::mesh_document::overwrite_geometry_question() const
{
	std::string text = m_dirty
		? std::format("This writes two files: the edited source {} and the geometry {}.", m_poly_tag.view(), m_geometry_tag.view())
		: std::format("This writes the geometry {} from the source {}.", m_geometry_tag.view(), m_poly_tag.view());
	if (m_geometry_mismatch) {
		text += std::format(" {} was not made from this source: it is replaced.", m_geometry_tag.view());
	}
	return text + " Continue?";
}

void edt::mesh_document::sync_preview_node()
{
	entt::registry* reg = registry();
	scn::ecs_assembler* assembler = m_services.assembler();
	if (!reg || !assembler || !m_preview_prefab || m_root == entt::null || !reg->valid(m_root)) return;
	mesh::material_range whole;
	bool first = true;
	for (const mesh::material_range& range : m_baked.ranges) {
		if (first) {
			whole = range;
			first = false;
			continue;
		}
		whole.vx_begin = std::min(whole.vx_begin, range.vx_begin);
		whole.vx_end = std::max(whole.vx_end, range.vx_end);
		whole.ind_begin = std::min(whole.ind_begin, range.ind_begin);
		whole.ind_end = std::max(whole.ind_end, range.ind_end);
	}
	scn::prefab_desc::prefab_node& node = m_preview_prefab->get_root().children.front();
	node.components["mesh"].overrides = mesh::mesh_fields(m_preview_tag.view(), whole);
	scn::reconcile(*assembler, *reg, m_root, m_preview_prefab->get_root(), scn::reconcile_mode::reload, m_poly_tag);
}
