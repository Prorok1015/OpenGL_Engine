#pragma once
#include "edt_panel_base.h"
#include <snk/ecs/ecs_entity.h>
#include <snk/resource/res_tag.h>

#include "edt_input_binding.h"
#include "edt_transform_gizmo.hpp"
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <span>
#include <cstdint>
#include <vector>
#include <string>

namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace scn { struct skeleton_component; }

namespace edt
{
	class viewport_panel : public panel_base
	{
	public:
		viewport_panel(rnd::render_system& rnd, gui::gui_system& gui);


		void set_registry_getter(std::function<entt::registry*()> getter);
		void set_selected_entity(ecs::entity ent) { m_selected = ent; }

		// The viewport feeds the context `target` names (the active world of the level) while hovered.
		void set_input_router(inp::input_router* router, input_target_getter target) {
			m_router = router;
			m_input_target = std::move(target);
		}

		// Called when a file is dropped onto the viewport from the Asset Browser.
		// Argument: absolute filesystem path of the dropped asset.
		void set_on_asset_dropped(std::function<void(const std::string&)> cb) { m_on_asset_dropped = std::move(cb); }

		void set_on_transform_committed(std::function<void(std::span<const uint64_t>, glm::vec3, glm::vec3, glm::vec3)> cb) { m_on_transform_committed = std::move(cb); }

		// The texture the editor camera renders to; the "scene" view of viewport.capture.
		static const res::tag& render_target_tag();

		glm::vec2 get_viewport_pos() const { return m_image_pos; }
		glm::vec2 get_viewport_size() const { return m_viewport_size; }
		bool is_focused() const { return m_focused; }

	protected:
		void on_render() override;

	private:
		void render_toolbar();
		void render_fps_overlay();
		void render_orientation_gizmo(entt::registry& reg);
		void render_transform_gizmo(entt::registry& reg, const glm::mat4& view, const glm::mat4& proj);
		scn::skeleton_component* find_skeleton_for_selected(entt::registry& reg);


		rnd::render_system& m_rnd;
		gui::gui_system&    m_gui;

		std::function<entt::registry*()> m_get_registry;
		inp::input_router* m_router = nullptr;
		input_target_getter m_input_target;
		ecs::entity m_selected = entt::null;
		glm::vec2 m_image_pos = {};      // screen pos of the scene image (after toolbar)
		glm::vec2 m_viewport_size = {800.f, 600.f};
		bool m_focused = false;
		int            m_gizmo_op     = 0;            // 0=translate, 1=rotate, 2=scale
		entt::entity   m_rotate_ent   = entt::null;   // gizmo rotate ring state
		int            m_active_ring  = -1;
		float m_fps = 0.f;
		float m_fps_timer = 0.f;
		int m_frame_count = 0;
		std::function<void(const std::string&)> m_on_asset_dropped;
		std::function<void(std::span<const uint64_t>, glm::vec3, glm::vec3, glm::vec3)> m_on_transform_committed;
	};
}
