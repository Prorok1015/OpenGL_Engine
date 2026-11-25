#include "edt_preview_stage.h"
#include "edt_editor_camera.h"
#include "edt_input_binding.h"
#include "edt_input_manager.h"
#include "edt_prefab_document.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <boost/json.hpp>
#include <imgui.h>
#include <atomic>
#include <format>
#include <string>
#include <utility>

namespace json = boost::json;

namespace
{
	const res::tag k_template_tag{ "edt://templates/prefab_preview.desc" };

	// The unit shapes are about 1 across: the camera starts far enough to see all of one, from above so a plane shows.
	constexpr float k_camera_distance = 2.2f;
	constexpr float k_camera_pitch_degrees = -30.f;
}

edt::preview_stage::preview_stage(editor_services& services, preview_content& content, preview_view view)
	: m_services(services)
	, m_content(content)
	, m_view(view)
{
	static std::atomic<std::uint64_t> s_stages{ 0 };
	m_serial = ++s_stages;
	m_key = std::format("memory://editor/__preview_stage/{}.desc", m_serial);
	if (rnd::render_system* render = m_view.render) {
		m_release_targets = [render](const std::vector<res::tag>& tags) {
			auto& textures = render->get_texture_manager();
			for (const res::tag& tag : tags) textures.remove(tag);
		};
	}
}

edt::preview_stage::~preview_stage()
{
	close();
}

void edt::preview_stage::update()
{
	const preview_content::state state = m_content.update();
	if (state == preview_content::state::none) {
		if (m_has_world || m_content_live) close();
		return;
	}
	if (state == preview_content::state::waiting) return;
	m_content_live = true;
	if (!m_has_world && !m_layer_parked && !m_failed) {
		open_world();
		if (!m_has_world) m_failed = true;
		return;
	}
	if (state == preview_content::state::changed) sync();
}

void edt::preview_stage::sync()
{
	entt::registry* reg = registry();
	scn::ecs_assembler* assembler = m_services.assembler();
	if (!reg || !assembler || !m_prefab || m_root == entt::null || !reg->valid(m_root)) return;
	m_content.refresh(m_prefab->get_root());
	scn::reconcile(*assembler, *reg, m_root, m_prefab->get_root(), scn::reconcile_mode::reload, res::tag{ m_working_tag });
}

void edt::preview_stage::set_sky(bool shown)
{
	if (shown == m_sky) return;
	m_sky = shown;
	apply_sky();
}

entt::registry* edt::preview_stage::registry()
{
	if (!m_has_world) return nullptr;
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return nullptr;
	scn::world_context* preview = ctx_mgr->find_preview(m_key);
	return preview ? &preview->state() : nullptr;
}

scn::camera_component* edt::preview_stage::camera()
{
	entt::registry* reg = registry();
	if (!reg) return nullptr;
	for (const entt::entity entity : reg->view<scn::camera_component>()) {
		scn::camera_component& found = reg->get<scn::camera_component>(entity);
		if (!found.color_targets.empty() && found.color_targets.front().view() == m_rt_color) return &found;
	}
	return nullptr;
}

void edt::preview_stage::set_viewport_size(int width, int height)
{
	if (scn::camera_component* shown = camera()) {
		shown->m_viewport.size = glm::ivec2{ width, height };
	}
}

void edt::preview_stage::park()
{
	set_viewport_size(0, 0);
}

void edt::preview_stage::draw_image()
{
	const ImVec2 size = ImGui::GetContentRegionAvail();
	if (size.x <= 0 || size.y <= 0) {
		park();
		return;
	}
	if (!camera()) {
		ImGui::TextDisabled("(no preview)");
		return;
	}
	if (!m_view.render || !m_view.gui) {
		park();
		return;
	}
	const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
	const glm::ivec2 pixels = edt::to_framebuffer_size({ size.x, size.y }, { fb_scale.x, fb_scale.y });
	set_viewport_size(pixels.x, pixels.y);

	auto& textures = m_view.render->get_texture_manager();
	const res::tag target{ m_rt_color };
	if (const auto texture = textures.binding(textures.id_of(target)).handle) {
		ImTextureID tex_id = m_view.gui->get_backend_interface()->to_imgui(texture);
		const ImVec2 image_pos = ImGui::GetCursorScreenPos();
		ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));
		ImGui::SetCursorScreenPos(image_pos);
		ImGui::InvisibleButton("##preview_stage_input", size);
		bind_item_input(m_view.input, inp::input_target{ .set = m_set_name, .context = std::string{ context_name() } });
	} else {
		ImGui::TextDisabled("(waiting for render target...)");
	}
}

void edt::preview_stage::open_world()
{
	m_has_world = false;
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || !m_services.sfactory() || !m_services.assembler()) return;
	if (!m_services.resources() || !m_services.writable_resources() || !m_services.descs()) return;

	const auto template_text = m_services.resources()->require_sync<res::text_resource>(k_template_tag);
	if (!template_text.is_ready()) {
		egLOG_ERROR("Editor/Preview", "No preview: the preview template could not be loaded");
		return;
	}
	json::object world_json;
	try {
		json::parse_options options;
		options.max_depth = 128;
		world_json = json::parse(template_text->str(), {}, options).as_object();
	} catch (const std::exception&) {
		egLOG_ERROR("Editor/Preview", "No preview: the preview template is not JSON");
		return;
	}
	const res::tag world_tag{ m_key };
	m_world_tag = world_tag;
	m_services.writable_resources()->store(world_tag, m_services.descs()->serialize_to_bytes(json::value(std::move(world_json))));
	m_services.resources()->signal_changed(world_tag);
	const auto world_res = m_services.resources()->require_sync<scn::world_desc>(world_tag);
	if (!world_res.is_ready()) {
		egLOG_ERROR("Editor/Preview", "No preview: the preview world could not be made");
		return;
	}

	scn::world_context& preview = ctx_mgr->create_preview(m_key, context_name(), world_res->get_systems());
	m_set_name = scn::context_manager::preview_set_name(m_key);
	entt::registry& reg = preview.state();
	const entt::entity template_root = reg.create();
	m_services.assembler()->spawn_from_desc(reg, template_root, *world_res, context_name());

	m_rt_color = std::format("memory://__preview_camera_rt/{}", m_serial);
	m_rt_depth = std::format("memory://__preview_depth_rt/{}", m_serial);
	m_rt_accum = std::format("memory://__preview_tp_accum_rt/{}", m_serial);
	m_rt_reveal = std::format("memory://__preview_tp_reveal_rt/{}", m_serial);

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
	reg.emplace<scn::name_component>(cam, "Preview Camera");
	reg.emplace<scn::depth_level>(cam);

	static std::atomic<std::uint64_t> s_worlds{ 0 };
	m_working_tag = std::format("memory://editor/__preview_working/{}", ++s_worlds);
	m_prefab = std::make_unique<scn::prefab_desc>();
	m_prefab->get_root().name = "preview";
	m_content.build(m_prefab->get_root());
	m_root = reg.create();
	scn::assemble_prefab(*m_services.assembler(), reg, m_root, *m_prefab, m_prefab->get_root().name, res::tag{ m_working_tag });
	m_has_world = true;
	apply_sky();
}

void edt::preview_stage::apply_sky()
{
	entt::registry* reg = registry();
	if (!reg) return;
	std::vector<entt::entity> skies;
	for (const entt::entity entity : reg->view<scn::sky_component>()) skies.push_back(entity);
	for (const entt::entity entity : skies) scn::set_visible(*reg, entity, m_sky);
}

void edt::preview_stage::release_world()
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

void edt::preview_stage::rebuild_world()
{
	m_layer_parked = false;
	m_failed = false;
	// A stage that showed nothing yet (or whose content has nothing to show) waits for update(), which asks the content.
	if (m_has_world || !m_content_live) return;
	open_world();
	if (!m_has_world) m_failed = true;
}

void edt::preview_stage::release_resources()
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

void edt::preview_stage::close()
{
	release_world();
	m_layer_parked = false;
	release_resources();
	m_content.release();
	m_content_live = false;
}
