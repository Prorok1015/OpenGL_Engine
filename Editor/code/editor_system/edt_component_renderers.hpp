#pragma once
#include "edt_inspector_panel.h"
#include "eng_transform_3d.hpp"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "scn_model.h"
#include "ecs_event.hpp"
#include <imgui.h>
#include <functional>

// Registers all built-in component renderers and creators into an inspector_panel.
// Call once after the panel is constructed, before levels are loaded.

namespace edt
{
	// ─── helpers ──────────────────────────────────────────────────────────────

	// Recursively apply / remove a playable_animation_component on every node
	// that owns a keyframes_component (i.e. the actual animated bones).
	inline void apply_animation_to_tree(entt::registry& reg, entt::entity ent, const scn::animation* anim)
	{
		if (reg.all_of<scn::keyframes_component>(ent)) {
			if (anim)
				reg.emplace_or_replace<scn::playable_animation_component>(
					ent, anim->name, anim->duration, anim->ticks_per_second);
			else
				reg.remove<scn::playable_animation_component>(ent);
		}
		if (const auto* ch = reg.try_get<scn::children_component>(ent))
			for (entt::entity child : ch->children)
				apply_animation_to_tree(reg, child, anim);
	}

	// ─── registration ─────────────────────────────────────────────────────────

	inline void register_builtin_component_renderers(inspector_panel& panel)
	{
		// --- Name ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::name_component>(e))
				return;
			if (ImGui::CollapsingHeader("Name", ImGuiTreeNodeFlags_DefaultOpen)) {
				auto& n = reg.get<scn::name_component>(e);
				char buf[256];
				strncpy(buf, n.name.c_str(), sizeof(buf) - 1);
				buf[sizeof(buf) - 1] = '\0';
				if (ImGui::InputText("##name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
					n.name = buf;
			}
		});

		// --- Transform (scn::local_transform) ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::local_transform>(e))
				return;
			if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
				auto& lt = reg.get<scn::local_transform>(e);
				eng::transform3d t{ lt.local };

				glm::vec3 position = t.get_pos();
				if (ImGui::DragFloat3("Position", &position.x, 0.1f)) {
					t.set_pos(position);
					lt.local = t.to_matrix();
					reg.ctx().get<ecs::event<scn::transform_updated>>().emit(e);
				}

				glm::vec3 rotation_deg = t.get_angles_degrees();
				if (ImGui::DragFloat3("Rotation", &rotation_deg.x, 0.5f)) {
					t.set_euler_angles(glm::radians(rotation_deg));
					lt.local = t.to_matrix();
					reg.ctx().get<ecs::event<scn::transform_updated>>().emit(e);
				}

				glm::vec3 scale = t.get_scale();
				if (ImGui::DragFloat3("Scale", &scale.x, 0.01f, 0.001f, 100.f)) {
					t.set_scale(scale);
					lt.local = t.to_matrix();
					reg.ctx().get<ecs::event<scn::transform_updated>>().emit(e);
				}
			}
		});

		// --- Renderable toggle ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			// show only if the entity has at least a mesh component (otherwise it's noise)
			if (!reg.all_of<scn::mesh_component>(e) && !reg.all_of<scn::camera_component>(e))
				return;
			if (ImGui::CollapsingHeader("Renderable", ImGuiTreeNodeFlags_DefaultOpen)) {
				bool visible = reg.all_of<scn::renderable>(e);
				if (ImGui::Checkbox("Visible", &visible)) {
					if (visible) reg.emplace<scn::renderable>(e);
					else         reg.remove<scn::renderable>(e);
				}
			}
		});

		// --- Mesh ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::mesh_component>(e))
				return;
			if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
				const auto& m = reg.get<scn::mesh_component>(e);
				ImGui::TextDisabled("Vertices : %zu", m.mesh.get_vertices_count());
				ImGui::TextDisabled("Indices  : %zu", m.mesh.get_indices_count());
			}
		});

		// --- Material ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::material_desc_component>(e))
				return;
			if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
				const auto& mat = reg.get<scn::material_desc_component>(e);
				if (mat.mlt_desc.is_ready())
					ImGui::TextDisabled("%s", mat.mlt_desc.get_tag().view().data());
				else
					ImGui::TextDisabled("(loading…)");
			}
		});

		// --- Animations ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::animations_component>(e))
				return;
			if (ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen)) {
				const auto& ac = reg.get<scn::animations_component>(e);

				// Determine currently playing animation name (if any)
				const scn::playable_animation_component* playing =
					reg.try_get<scn::playable_animation_component>(e);
				const std::string current_name = playing ? playing->name : "NONE";

				if (ImGui::BeginCombo("Animation", current_name.c_str())) {
					// NONE entry
					bool is_none = !playing;
					if (ImGui::Selectable("NONE", is_none) && !is_none)
						apply_animation_to_tree(reg, e, nullptr);

					for (const auto& anim : ac.animations) {
						bool selected = (current_name == anim.name);
						if (ImGui::Selectable(anim.name.c_str(), selected) && !selected)
							apply_animation_to_tree(reg, e, &anim);
						if (selected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}

				if (playing) {
					float progress = (playing->duration > 0.f)
						? playing->current_tick / playing->duration : 0.f;
					ImGui::ProgressBar(progress, ImVec2(-1.f, 0.f));
				}
			}
		});

		// --- Camera ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::camera_component>(e))
				return;
			if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
				auto& cam = reg.get<scn::camera_component>(e);
				ImGui::DragFloat("FOV",  &cam.fov,           0.5f,   1.0f,     179.0f);
				ImGui::DragFloat("Near", &cam.near_distance, 0.001f, 0.0001f,  10.0f, "%.4f");
				ImGui::DragFloat("Far",  &cam.far_distance,  1.0f,   1.0f,     100000.0f);
			}
		});

		// --- Directional Light ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::directional_light>(e))
				return;
			if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
				auto& light = reg.get<scn::directional_light>(e);

				ImGui::ColorEdit3("Diffuse",  &light.diffuse.r);
				ImGui::ColorEdit3("Ambient",  &light.ambient.r);
				ImGui::ColorEdit3("Specular", &light.specular.r);

				glm::vec3 dir = glm::vec3(light.direction);
				if (ImGui::DragFloat3("Direction", &dir.x, 0.01f, -1.f, 1.f)) {
					float len = glm::length(dir);
					if (len > 0.0001f)
						light.direction = glm::vec4(dir / len, 0.f);
				}
			}
		});

		// --- Camera Controller ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::mouse_controller_component>(e))
				return;
			if (ImGui::CollapsingHeader("Camera Controller", ImGuiTreeNodeFlags_DefaultOpen)) {
				auto& ctrl = reg.get<scn::mouse_controller_component>(e);
				ImGui::DragFloat("Move Speed", &ctrl.movement_speed, 0.1f, 0.01f, 100.f);
				float rot_deg = glm::degrees(ctrl.rotating_speed);
				if (ImGui::DragFloat("Rotate Speed (deg/s)", &rot_deg, 1.f, 1.f, 720.f))
					ctrl.rotating_speed = glm::radians(rot_deg);
			}
		});

		// --- Bone (read-only debug info) ---
		panel.add_component_renderer([](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::bone_component>(e))
				return;
			if (ImGui::CollapsingHeader("Bone", ImGuiTreeNodeFlags_DefaultOpen)) {
				const auto& bone = reg.get<scn::bone_component>(e);
				ImGui::TextDisabled("Index: %d", bone.index);
			}
		});

		// ─── Add Component creators ────────────────────────────────────────────
		panel.add_component_creator("Transform", [](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::local_transform>(e))
				reg.emplace<scn::local_transform>(e);
		});
		panel.add_component_creator("Camera", [](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::camera_component>(e))
				reg.emplace<scn::camera_component>(e);
		});
		panel.add_component_creator("Directional Light", [](entt::registry& reg, entt::entity e) {
			if (!reg.all_of<scn::directional_light>(e))
				reg.emplace<scn::directional_light>(e);
		});
	}
}
