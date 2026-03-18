#pragma once
#include "scn_model.h"
#include "ecs_event.hpp"
#include "ecs_entity.h"
#include <imgui.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

// Minimal interactive transform gizmo rendered via ImDrawList.
// Supports Translate (T), Rotate (R), and Scale (S) modes.

namespace edt
{
	// -------------------------------------------------------------------------
	// Visual constants
	// -------------------------------------------------------------------------
	static constexpr float GIZMO_ARROW_PX    = 70.f;
	static constexpr float GIZMO_HANDLE_R    = 8.f;
	static constexpr float GIZMO_LINE_W      = 2.5f;
	static constexpr float GIZMO_RING_PX     = 55.f;   // base ring radius in pixels
	static constexpr float GIZMO_RING_STEP   = 12.f;   // extra pixels per ring
	static constexpr float GIZMO_RING_HIT    = 8.f;    // ring outline hit threshold
	static constexpr float SCALE_SENSITIVITY = 0.007f;
	static constexpr int   RING_SEGS         = 64;

	static constexpr ImU32 GIZMO_X     = 0xFF2244EE;
	static constexpr ImU32 GIZMO_Y     = 0xFF22CC44;
	static constexpr ImU32 GIZMO_Z     = 0xFFCC4422;
	static constexpr ImU32 GIZMO_HOVER = 0xFFFFFFFF;

	// -------------------------------------------------------------------------
	// Project a world-space point to screen. Returns false if behind camera.
	// -------------------------------------------------------------------------
	inline bool world_to_screen(
		const glm::vec3& world_pos,
		const glm::mat4& vp_mat,
		float vp_x, float vp_y, float vp_w, float vp_h,
		ImVec2& out)
	{
		glm::vec4 clip = vp_mat * glm::vec4(world_pos, 1.f);
		if (clip.w < 0.001f)
			return false;
		glm::vec3 ndc = glm::vec3(clip) / clip.w;
		out = {
			(ndc.x + 1.f) * 0.5f * vp_w + vp_x,
			(1.f - ndc.y) * 0.5f * vp_h + vp_y
		};
		return true;
	}

	// -------------------------------------------------------------------------
	// Axis info for translate/scale arrows
	// -------------------------------------------------------------------------
	struct axis_screen_info
	{
		ImVec2 dir            = {0.f, 0.f};
		float  screen_per_unit = 0.f;
		ImVec2 tip            = {0.f, 0.f};
		bool   valid          = false;
	};

	inline axis_screen_info compute_axis(ImVec2 origin, ImVec2 axis_1unit, float display_len)
	{
		float dx = axis_1unit.x - origin.x;
		float dy = axis_1unit.y - origin.y;
		float len = sqrtf(dx * dx + dy * dy);
		if (len < 0.5f) return {};
		axis_screen_info info;
		info.valid          = true;
		info.screen_per_unit = len;
		info.dir            = {dx / len, dy / len};
		info.tip            = {origin.x + info.dir.x * display_len,
		                       origin.y + info.dir.y * display_len};
		return info;
	}

	// -------------------------------------------------------------------------
	// Translate axis arrow
	// -------------------------------------------------------------------------
	inline glm::vec3 draw_translate_axis(
		ImDrawList* dl, ImVec2 origin, ImVec2 axis_1unit,
		glm::vec3 world_axis, ImU32 color, int btn_id,
		bool* committed = nullptr)
	{
		auto ax = compute_axis(origin, axis_1unit, GIZMO_ARROW_PX);
		if (!ax.valid) return {};

		ImGui::SetCursorScreenPos({ax.tip.x - GIZMO_HANDLE_R, ax.tip.y - GIZMO_HANDLE_R});
		ImGui::PushID(btn_id);
		ImGui::InvisibleButton("##ax", {GIZMO_HANDLE_R * 2.f, GIZMO_HANDLE_R * 2.f});
		if (ImGui::IsItemDeactivated() && committed) *committed = true;
		bool hovered = ImGui::IsItemHovered();
		bool active  = ImGui::IsItemActive();
		ImGui::PopID();

		ImU32 col = (hovered || active) ? GIZMO_HOVER : color;
		dl->AddLine(origin, ax.tip, col, GIZMO_LINE_W);
		ImVec2 perp = {-ax.dir.y * 5.f, ax.dir.x * 5.f};
		ImVec2 base = {ax.tip.x - ax.dir.x * 12.f, ax.tip.y - ax.dir.y * 12.f};
		dl->AddTriangleFilled(ax.tip,
			{base.x + perp.x, base.y + perp.y},
			{base.x - perp.x, base.y - perp.y}, col);
		dl->AddCircleFilled(ax.tip, GIZMO_HANDLE_R, col);

		if (active) {
			ImVec2 md = ImGui::GetIO().MouseDelta;
			float proj = md.x * ax.dir.x + md.y * ax.dir.y;
			return world_axis * (proj / ax.screen_per_unit);
		}
		return {};
	}

	// -------------------------------------------------------------------------
	// Scale axis handle (box tip)
	// -------------------------------------------------------------------------
	inline glm::vec3 draw_scale_axis(
		ImDrawList* dl, ImVec2 origin, ImVec2 axis_1unit,
		glm::vec3 world_axis, ImU32 color, int btn_id,
		bool* committed = nullptr)
	{
		auto ax = compute_axis(origin, axis_1unit, GIZMO_ARROW_PX);
		if (!ax.valid) return {};

		ImGui::SetCursorScreenPos({ax.tip.x - GIZMO_HANDLE_R, ax.tip.y - GIZMO_HANDLE_R});
		ImGui::PushID(btn_id);
		ImGui::InvisibleButton("##sc", {GIZMO_HANDLE_R * 2.f, GIZMO_HANDLE_R * 2.f});
		if (ImGui::IsItemDeactivated() && committed) *committed = true;
		bool hovered = ImGui::IsItemHovered();
		bool active  = ImGui::IsItemActive();
		ImGui::PopID();

		ImU32 col = (hovered || active) ? GIZMO_HOVER : color;
		dl->AddLine(origin, ax.tip, col, GIZMO_LINE_W);
		float hs = GIZMO_HANDLE_R;
		dl->AddRectFilled({ax.tip.x - hs, ax.tip.y - hs}, {ax.tip.x + hs, ax.tip.y + hs}, col);

		if (active) {
			ImVec2 md = ImGui::GetIO().MouseDelta;
			float proj = md.x * ax.dir.x + md.y * ax.dir.y;
			return world_axis * (proj * SCALE_SENSITIVITY);
		}
		return {};
	}

	// -------------------------------------------------------------------------
	// Public: draw transform gizmo. mode: 0=translate, 1=rotate, 2=scale.
	// Returns true if the entity transform was modified this frame.
	// -------------------------------------------------------------------------
	inline bool draw_transform_gizmo(
		entt::registry& reg,
		entt::entity ent,
		const glm::mat4& view,
		const glm::mat4& proj,
		float vp_x, float vp_y, float vp_w, float vp_h,
		int mode, // TODO: use enum class
		entt::entity& inout_rotate_ent,
		int& inout_active_ring,
		bool* committed = nullptr)
	{
		if (!reg.all_of<scn::local_transform>(ent))
			return false;

		auto& lt = reg.get<scn::local_transform>(ent);

		// Use world_transform for position and axis orientation.
		// lt.local is parent-local — wrong for children of rotated parents.
		glm::mat4 world_mat = lt.local;
		if (reg.all_of<scn::world_transform>(ent))
			world_mat = reg.get<scn::world_transform>(ent).world;

		const glm::vec3 world_pos = glm::vec3(world_mat[3]);

		// World-space axes of the object (from world matrix columns)
		const glm::vec3 world_x = glm::normalize(glm::vec3(world_mat[0]));
		const glm::vec3 world_y = glm::normalize(glm::vec3(world_mat[1]));
		const glm::vec3 world_z = glm::normalize(glm::vec3(world_mat[2]));

		// Parent world matrix — needed to convert world-space translation delta
		// back into parent-local space before writing into lt.local.
		glm::mat4 parent_world = glm::mat4(1.f); // identity for root entities
		if (reg.all_of<scn::parent_component>(ent)) {
			entt::entity parent_ent = reg.get<scn::parent_component>(ent).parent;
			if (reg.valid(parent_ent) && reg.all_of<scn::world_transform>(parent_ent))
				parent_world = reg.get<scn::world_transform>(parent_ent).world;
		}

		const glm::mat4 vp_mat = proj * view;

		ImVec2 screen_origin;
		if (!world_to_screen(world_pos, vp_mat, vp_x, vp_y, vp_w, vp_h, screen_origin))
			return false;

		// Project world-axis endpoints to screen
		ImVec2 sx, sy, sz;
		world_to_screen(world_pos + world_x, vp_mat, vp_x, vp_y, vp_w, vp_h, sx);
		world_to_screen(world_pos + world_y, vp_mat, vp_x, vp_y, vp_w, vp_h, sy);
		world_to_screen(world_pos + world_z, vp_mat, vp_x, vp_y, vp_w, vp_h, sz);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		bool modified  = false;

		// ----- TRANSLATE -----
		if (mode == 0) {
			glm::vec3 world_delta{};
			world_delta += draw_translate_axis(dl, screen_origin, sx, world_x, GIZMO_X, 200, committed);
			world_delta += draw_translate_axis(dl, screen_origin, sy, world_y, GIZMO_Y, 201, committed);
			world_delta += draw_translate_axis(dl, screen_origin, sz, world_z, GIZMO_Z, 202, committed);
			if (world_delta != glm::vec3(0)) {
				// world_delta is in world space; lt.local[3] is in parent-local space.
				// Convert: local_delta = inverse(parent_world) * world_delta
				glm::vec3 local_delta = glm::vec3(glm::inverse(parent_world) * glm::vec4(world_delta, 0.f));
				lt.local[3] += glm::vec4(local_delta, 0.f);
				modified = true;
			}
		}

		// ----- ROTATE -----
		else if (mode == 1)
		{
			// Each ring is a world-space circle projected to screen via world_to_screen.
			// ring_ax1/ring_ax2 are the two world axes spanning the ring's plane.
			// ring_axis is the rotation axis (normal to the plane).
			struct ring_def {
				glm::vec3 axis;   // rotation axis (in local coords: {1,0,0} = local X)
				glm::vec3 ax1;    // ring plane basis 1 (world-space local axis)
				glm::vec3 ax2;    // ring plane basis 2 (world-space local axis)
				ImU32     color;
			};
			// Ring plane axes are the object's local axes so rings match its orientation.
			// rotation axis stays {1,0,0}/{0,1,0}/{0,0,1} because lt.local*rot() applies
			// the rotation in local space (those are local-space axis coordinates).
			// Ring plane axes use world-space object axes so rings display correctly
			// when the entity (or its parent) is rotated.
			// rings[r].axis stays in local coords {1,0,0} etc. because rotation is
			// applied as lt.local * rot(), which rotates in the object's own local frame.
			const ring_def rings[3] = {
				{{1,0,0}, world_y, world_z, GIZMO_X},
				{{0,1,0}, world_x, world_z, GIZMO_Y},
				{{0,0,1}, world_x, world_y, GIZMO_Z},
			};

			// Estimate world units per screen pixel at entity depth
			// (average over the three projected axis vectors)
			float ppu = 0.f;
			{
				auto ppu_axis = [&](ImVec2 a) {
					float dx = a.x - screen_origin.x, dy = a.y - screen_origin.y;
					return sqrtf(dx*dx + dy*dy);
				};
				ppu = (ppu_axis(sx) + ppu_axis(sy) + ppu_axis(sz)) / 3.f;
			}
			if (ppu < 0.001f) ppu = 1.f;

			// Project ring points in 3D for correct elliptical shape
			ImVec2 ring_pts[3][RING_SEGS + 1];
			for (int r = 0; r < 3; ++r) {
				float world_r = (GIZMO_RING_PX + r * GIZMO_RING_STEP) / ppu;
				for (int i = 0; i <= RING_SEGS; ++i) {
					float a  = 2.f * 3.14159265f * i / RING_SEGS;
					glm::vec3 wp = world_pos
						+ world_r * (cosf(a) * rings[r].ax1 + sinf(a) * rings[r].ax2);
					if (!world_to_screen(wp, vp_mat, vp_x, vp_y, vp_w, vp_h, ring_pts[r][i]))
						ring_pts[r][i] = {-99999.f, -99999.f};
				}
			}

			// For each ring: find the minimum distance from the mouse to the ring outline
			ImVec2 mouse = ImGui::GetMousePos();
			float ring_dist[3] = {1e9f, 1e9f, 1e9f};
			for (int r = 0; r < 3; ++r) {
				for (int i = 0; i < RING_SEGS; ++i) {
					float mx = (ring_pts[r][i].x + ring_pts[r][i+1].x) * 0.5f;
					float my = (ring_pts[r][i].y + ring_pts[r][i+1].y) * 0.5f;
					float dx = mx - mouse.x, dy = my - mouse.y;
					ring_dist[r] = std::min(ring_dist[r], sqrtf(dx*dx + dy*dy));
				}
			}

			// Single InvisibleButton covering the outer ring + hit margin.
			// We manage which ring is "active" ourselves using caller-owned state
			// keyed by the selected entity, so switching entities resets the state.
			
			

			if (inout_rotate_ent != ent) {
				inout_rotate_ent  = ent;
				inout_active_ring = -1;
			}

			float btn_r = GIZMO_RING_PX + 2 * GIZMO_RING_STEP + GIZMO_RING_HIT;
			ImGui::SetCursorScreenPos({screen_origin.x - btn_r, screen_origin.y - btn_r});
			ImGui::PushID(300);
			ImGui::InvisibleButton("##rotall", {btn_r * 2.f, btn_r * 2.f});
			const bool btn_active  = ImGui::IsItemActive();
			const bool btn_clicked = ImGui::IsItemClicked();
			if (ImGui::IsItemDeactivated()) {
				inout_active_ring = -1;
				if (committed) *committed = true;
			}
			ImGui::PopID();

			// On first click: pick the ring closest to the mouse (if within hit radius)
			if (btn_clicked) {
				int best = 0;
				for (int r = 1; r < 3; ++r)
					if (ring_dist[r] < ring_dist[best]) best = r;
				inout_active_ring = (ring_dist[best] < GIZMO_RING_HIT) ? best : -1;
			}

			// Draw all rings
			for (int r = 0; r < 3; ++r) {
				bool near   = (ring_dist[r] < GIZMO_RING_HIT);
				bool active = (btn_active && inout_active_ring == r);
				ImU32 col   = (near || active) ? GIZMO_HOVER : rings[r].color;
				float lw    = active ? GIZMO_LINE_W * 2.f : GIZMO_LINE_W;
				for (int i = 0; i < RING_SEGS; ++i)
					dl->AddLine(ring_pts[r][i], ring_pts[r][i+1], col, lw);
			}

			// Apply rotation when dragging
			if (btn_active && inout_active_ring >= 0) {
				int r = inout_active_ring;

				// Screen-space tangent at the ring point closest to the mouse
				int closest_seg = 0;
				float min_d = 1e9f;
				for (int i = 0; i < RING_SEGS; ++i) {
					float dx = ring_pts[r][i].x - mouse.x;
					float dy = ring_pts[r][i].y - mouse.y;
					float d  = sqrtf(dx*dx + dy*dy);
					if (d < min_d) { min_d = d; closest_seg = i; }
				}
				int prev = (closest_seg - 1 + RING_SEGS) % RING_SEGS;
				int next = (closest_seg + 1) % RING_SEGS;
				float tx = ring_pts[r][next].x - ring_pts[r][prev].x;
				float ty = ring_pts[r][next].y - ring_pts[r][prev].y;
				float tlen = sqrtf(tx*tx + ty*ty);
				if (tlen > 0.001f) { tx /= tlen; ty /= tlen; }

				ImVec2 md = ImGui::GetIO().MouseDelta;
				float rot_delta = (md.x * tx + md.y * ty) * 0.015f;

				if (rot_delta != 0.f) {
					glm::vec3 pos3 = glm::vec3(lt.local[3]);
					glm::mat4 rot  = glm::rotate(glm::mat4(1.f), rot_delta, rings[r].axis);
					lt.local       = lt.local * rot;
					lt.local[3]    = glm::vec4(pos3, 1.f);
					modified       = true;
				}
			}
		}

		// ----- SCALE -----
		else if (mode == 2)
		{
			// Scale is applied in model space (lt.local * scale()), so the delta axes
			// must be model-space unit vectors {1,0,0}/{0,1,0}/{0,0,1}.
			// The screen projection (sx/sy/sz) still uses world axes for correct display.
			glm::vec3 scale_delta{};
			scale_delta += draw_scale_axis(dl, screen_origin, sx, {1,0,0}, GIZMO_X, 220, committed);
			scale_delta += draw_scale_axis(dl, screen_origin, sy, {0,1,0}, GIZMO_Y, 221, committed);
			scale_delta += draw_scale_axis(dl, screen_origin, sz, {0,0,1}, GIZMO_Z, 222, committed);
			if (scale_delta != glm::vec3(0)) {
				lt.local = lt.local * glm::scale(glm::mat4(1.f), glm::vec3(1.f) + scale_delta);
				modified = true;
			}
		}

		if (modified && reg.ctx().contains<ecs::event<scn::transform_updated>>())
			reg.ctx().get<ecs::event<scn::transform_updated>>().emit(ent);

		return modified;
	}
}
