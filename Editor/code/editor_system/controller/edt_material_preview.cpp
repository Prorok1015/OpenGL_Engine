#include "edt_material_preview.h"
#include "edt_data_document.h"
#include "edt_editor_camera.h"
#include "edt_material_window_model.h"
#include "edt_mesh_base_shapes.h"
#include "edt_prefab_document.h"
#include "edt_shape_menu_model.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <boost/json.hpp>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace json = boost::json;

namespace
{
	const res::tag k_template_tag{ "edt://templates/prefab_preview.desc" };
	const res::tag k_sky_shader{ "res://shaders/sky.shader.desc" };
	const res::tag k_sky_vertex{ "res://shaders/sky.vert" };

	std::string_view shape_name(edt::preview_shape shape)
	{
		switch (shape) {
		case edt::preview_shape::cube: return "box";
		case edt::preview_shape::plane: return "plane";
		case edt::preview_shape::sphere: break;
		}
		return "sphere";
	}

	// The unit shapes are about 1 across: the camera starts far enough to see all of one, from above so a plane shows.
	constexpr float k_camera_distance = 2.2f;
	constexpr float k_camera_pitch_degrees = -30.f;
}

std::string_view edt::preview_shape_label(preview_shape shape)
{
	switch (shape) {
	case preview_shape::cube: return "Cube";
	case preview_shape::plane: return "Plane";
	case preview_shape::sphere: break;
	}
	return "Sphere";
}

std::string edt::preview_shape_geometry(preview_shape shape)
{
	return std::format("res://shapes/{}.geom.desc", shape_name(shape));
}

bool edt::is_sky_material(const material_shader& shader)
{
	return shader.shader == k_sky_shader || shader.vertex == k_sky_vertex;
}

edt::material_preview::material_preview(data_document& document, editor_services& services)
	: m_document(document)
	, m_services(services)
{
	static std::atomic<std::uint64_t> s_previews{ 0 };
	m_serial = ++s_previews;
	m_material_tag = res::tag{ std::format("memory://editor/material/{}.desc", m_serial) };
	m_key = std::format("memory://editor/__material_preview/{}.desc", m_serial);
}

edt::material_preview::~material_preview()
{
	close();
}

void edt::material_preview::update(const material_shader& shader)
{
	m_sky_material = is_sky_material(shader);
	if (m_sky_material) {
		if (m_has_world || m_material_written) close();
		return;
	}
	const bool changed = !m_material_written || m_written_generation != m_document.generation();
	if (changed && !write_material()) return;
	if (!m_has_world && !m_layer_parked && !m_failed) {
		open_world();
		if (!m_has_world) m_failed = true;
		return;
	}
	if (changed) sync_node();
}

void edt::material_preview::set_shape(preview_shape shape)
{
	if (shape == m_shape) return;
	m_shape = shape;
	sync_node();
}

void edt::material_preview::set_sky(bool shown)
{
	if (shown == m_sky) return;
	m_sky = shown;
	apply_sky();
}

bool edt::material_preview::write_material()
{
	res::resource_system* writer = m_services.writable_resources();
	desc::desc_system* descs = m_services.descs();
	if (!writer || !descs) return false;
	const json::value shown = m_document.shown();
	json::object body = shown.is_object() ? shown.as_object() : json::object{};
	std::vector<std::string> headers;
	for (const auto& entry : body) {
		const std::string_view key(entry.key().data(), entry.key().size());
		if (key.starts_with("__")) headers.emplace_back(key);
	}
	// The parent chain is folded into what is shown: the copy has no __parent, and no __guid of the file it copies.
	for (const std::string& key : headers) body.erase(key);
	body["__type"] = m_document.type();
	body["__version"] = m_document.current_version();
	const json::value content(std::move(body));
	try {
		writer->store(m_material_tag, descs->serialize_to_bytes(content));
	} catch (const std::exception& failure) {
		egLOG_WARN("Editor/Material", "The material preview cannot be written: {}", failure.what());
		return false;
	}
	writer->signal_changed(m_material_tag);
	m_material_written = true;
	m_written_generation = m_document.generation();
	return true;
}

void edt::material_preview::erase_material()
{
	if (!m_material_written) return;
	if (res::resource_system* writer = m_services.writable_resources()) {
		writer->erase(m_material_tag);
		writer->signal_changed(m_material_tag);
	}
	m_material_written = false;
}

entt::registry* edt::material_preview::registry()
{
	if (!m_has_world) return nullptr;
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return nullptr;
	scn::world_context* preview = ctx_mgr->find_preview(m_key);
	return preview ? &preview->state() : nullptr;
}

scn::camera_component* edt::material_preview::camera()
{
	entt::registry* reg = registry();
	if (!reg) return nullptr;
	for (const entt::entity entity : reg->view<scn::camera_component>()) {
		scn::camera_component& found = reg->get<scn::camera_component>(entity);
		if (!found.color_targets.empty() && found.color_targets.front().view() == m_rt_color) return &found;
	}
	return nullptr;
}

entt::entity edt::material_preview::shape_entity()
{
	entt::registry* reg = registry();
	if (!reg) return entt::null;
	const auto shapes = reg->view<scn::mesh_component>();
	if (shapes.begin() == shapes.end()) return entt::null;
	return *shapes.begin();
}

void edt::material_preview::park()
{
	if (scn::camera_component* shown = camera()) {
		shown->m_viewport.size = decltype(shown->m_viewport.size){};
	}
}

void edt::material_preview::open_world()
{
	m_has_world = false;
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || !m_services.sfactory() || !m_services.assembler()) return;
	if (!m_services.resources() || !m_services.writable_resources() || !m_services.descs()) return;

	const auto template_text = m_services.resources()->require_sync<res::text_resource>(k_template_tag);
	if (!template_text.is_ready()) {
		egLOG_ERROR("Editor/Material", "No material preview: the preview template could not be loaded");
		return;
	}
	json::object world_json;
	try {
		json::parse_options options;
		options.max_depth = 128;
		world_json = json::parse(template_text->str(), {}, options).as_object();
	} catch (const std::exception&) {
		egLOG_ERROR("Editor/Material", "No material preview: the preview template is not JSON");
		return;
	}
	const res::tag world_tag{ m_key };
	m_world_tag = world_tag;
	m_services.writable_resources()->store(world_tag, m_services.descs()->serialize_to_bytes(json::value(std::move(world_json))));
	m_services.resources()->signal_changed(world_tag);
	const auto world_res = m_services.resources()->require_sync<scn::world_desc>(world_tag);
	if (!world_res.is_ready()) {
		egLOG_ERROR("Editor/Material", "No material preview: the preview world could not be made");
		return;
	}

	scn::world_context& preview = ctx_mgr->create_preview(m_key, context_name(), world_res->get_systems());
	m_set_name = scn::context_manager::preview_set_name(m_key);
	entt::registry& reg = preview.state();
	const entt::entity template_root = reg.create();
	m_services.assembler()->spawn_from_desc(reg, template_root, *world_res, context_name());

	m_rt_color = std::format("memory://__material_camera_rt/{}", m_serial);
	m_rt_depth = std::format("memory://__material_depth_rt/{}", m_serial);
	m_rt_accum = std::format("memory://__material_tp_accum_rt/{}", m_serial);
	m_rt_reveal = std::format("memory://__material_tp_reveal_rt/{}", m_serial);

	editor_camera_state view;
	view.position = glm::vec3{ 0.f };
	view.rotation = glm::vec3{ glm::radians(k_camera_pitch_degrees), 0.f, 0.f };
	view.distance = k_camera_distance;
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
	reg.emplace<scn::name_component>(cam, "Material Camera");
	reg.emplace<scn::depth_level>(cam);

	static std::atomic<std::uint64_t> s_worlds{ 0 };
	m_working_tag = std::format("memory://editor/__material_working/{}", ++s_worlds);
	m_prefab = std::make_unique<scn::prefab_desc>();
	m_prefab->get_root().name = "material";
	m_prefab->get_root().children.push_back(mesh::mesh_node("shape", node_fields()));
	m_node_shape = m_shape;
	m_root = reg.create();
	scn::assemble_prefab(*m_services.assembler(), reg, m_root, *m_prefab, m_prefab->get_root().name, res::tag{ m_working_tag });
	m_has_world = true;
	apply_sky();
}

json::object edt::material_preview::node_fields() const
{
	const mesh::base_shape* shape = mesh::find_base_shape(shape_name(m_shape));
	ASSERT_MSG(shape != nullptr, "the preview shapes are base shapes of core/res/shapes");
	json::object fields = shape ? mesh::mesh_fields(mesh::base_shape_path(*shape), mesh::base_shape_range(*shape)) : json::object{};
	fields["material"] = std::string(m_material_tag.view());
	return fields;
}

void edt::material_preview::sync_node()
{
	entt::registry* reg = registry();
	scn::ecs_assembler* assembler = m_services.assembler();
	if (!reg || !assembler || !m_prefab || m_root == entt::null || !reg->valid(m_root)) return;
	// The range of a base shape is a bake of it: made again only when the shape changes, not at every edit.
	if (m_node_shape != m_shape) {
		m_prefab->get_root().children.front().components["mesh"].overrides = node_fields();
		m_node_shape = m_shape;
	}
	scn::reconcile(*assembler, *reg, m_root, m_prefab->get_root(), scn::reconcile_mode::reload, res::tag{ m_working_tag });
}

void edt::material_preview::apply_sky()
{
	entt::registry* reg = registry();
	if (!reg) return;
	std::vector<entt::entity> skies;
	for (const entt::entity entity : reg->view<scn::sky_component>()) skies.push_back(entity);
	for (const entt::entity entity : skies) scn::set_visible(*reg, entity, m_sky);
}

void edt::material_preview::release_world()
{
	if (m_has_world) {
		if (auto ctx_mgr = m_services.ctx_manager.lock()) ctx_mgr->destroy_preview(m_key);
	}
	m_has_world = false;
	m_layer_parked = true;
	m_root = entt::null;
	m_prefab.reset();
	m_set_name.clear();
}

void edt::material_preview::rebuild_world()
{
	m_layer_parked = false;
	m_failed = false;
	// A preview that showed nothing yet (or a sky material) waits for update(), which knows the shader.
	if (m_has_world || !m_material_written || m_sky_material) return;
	open_world();
	if (!m_has_world) m_failed = true;
}

void edt::material_preview::release_resources()
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

void edt::material_preview::close()
{
	release_world();
	m_layer_parked = false;
	release_resources();
	erase_material();
}
