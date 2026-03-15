#pragma once
#include "common.h"
#include <imgui.h>

namespace edt::imgui {
	namespace details {
		static struct internal_config {
			glm::vec2 pos{ 0 };
			float size = 100.f;
			ImDrawList* mDrawList = nullptr;
		} config;

		inline bool is_hit_circle(const glm::vec2& center, const float radius, const ImVec2& point)
		{
			return (point.x - center.x) * (point.x - center.x) + (point.y - center.y) * (point.y - center.y) <= radius * radius;
		}

		inline void draw_positive_line(const glm::vec2& center, const glm::vec2& axis, const ImU32 color, const float radius, const float thickness, const char* text, const bool selected)
		{
			const auto end = ImVec2{ center.x + axis.x, center.y + axis.y };
			const auto im_center = ImVec2{ center.x, center.y };
			details::config.mDrawList->AddLine(im_center, end, color, thickness);
			details::config.mDrawList->AddCircleFilled(end, radius, color);
			const auto textPosX = ImVec2{ floor(end.x - 3.0f), floor(end.y - 6.0f) };
			if (selected) {
				details::config.mDrawList->AddCircle(end, radius, IM_COL32_WHITE, 0, 1.1f);
				details::config.mDrawList->AddText(textPosX, IM_COL32_WHITE, text);
			}
			else details::config.mDrawList->AddText(textPosX, IM_COL32_BLACK, text);
		}

		inline void draw_negative_line(const glm::vec2& center, const glm::vec2& axis, const ImU32 color, const float radius, const bool selected)
		{
			const auto end = ImVec2{ center.x - axis.x, center.y - axis.y };
			details::config.mDrawList->AddCircleFilled(end, radius, color);
			if (selected) {
				details::config.mDrawList->AddCircle(end, radius, IM_COL32_WHITE, 0, 1.1f);
			}
		}
	}

	static struct style_config {
		// in relation to half the rect size
		float line_thickness_scale = 0.017f;
		float axis_length_scale = 0.33f;
		float positive_radius_scale = 0.075f;
		float negative_radius_scale = 0.05f;
		float hover_circle_radius_scale = 0.88f;
		ImU32 circle_front_color_x = IM_COL32(255, 54, 83, 255);
		ImU32 circle_back_color_x = IM_COL32(154, 57, 71, 255);
		ImU32 circle_front_color_y = IM_COL32(138, 219, 0, 255);
		ImU32 circle_back_color_y = IM_COL32(98, 138, 34, 255);
		ImU32 circle_front_color_z = IM_COL32(44, 143, 255, 255);
		ImU32 circle_back_color_z = IM_COL32(52, 100, 154, 255);
		ImU32 hover_circle_color = IM_COL32(100, 100, 100, 130);
	} style;

	inline void set_view_area(const float x, const float y, const float size)
	{
		details::config.pos.x = x;
		details::config.pos.y = y;
		details::config.size = size;
	}

	inline void set_draw_list(ImDrawList* drawlist = nullptr)
	{
		details::config.mDrawList = drawlist ? drawlist : ImGui::GetWindowDrawList();
	}

	inline bool draw_gizmo(const glm::mat4& view, const glm::mat4 proj, const bool interactive = true)
	{
		const float size = details::config.size;
		const float half_size = size * 0.5f;
		const auto center = details::config.pos + half_size;
		glm::mat4 VP = proj * view;
		// Flip Y
		VP[0][1] *= -1;
		VP[1][1] *= -1;
		VP[2][1] *= -1;
		VP[3][1] *= -1;
		// correction for non-square aspect ratio
		const float aspect_ratio = proj[1][1] / proj[0][0];
		VP[0][0] *= aspect_ratio;
		VP[2][0] *= aspect_ratio;
		// axis
		const float axis_length = size * style.axis_length_scale;
		const auto axis_x = VP * glm::vec4{ axis_length, 0, 0, 0 };
		const auto axis_y = VP * glm::vec4{ 0, axis_length, 0, 0 };
		const auto axis_z = VP * glm::vec4{ 0, 0, axis_length, 0 };
		
		const ImVec2 mouse_pos = ImGui::GetIO().MousePos;

		const float hover_circle_radius = half_size * style.hover_circle_radius_scale;
		set_draw_list(details::config.mDrawList);
		if (interactive && details::is_hit_circle(center, hover_circle_radius, mouse_pos)) {
			details::config.mDrawList->AddCircleFilled(ImVec2{ center.x, center.y }, hover_circle_radius, style.hover_circle_color);
		}

		const float positive_radius = size * style.positive_radius_scale;
		const float negative_radius = size * style.negative_radius_scale;
		// sort axis based on distance
		// 0 : +x axis, 1 : +y axis, 2 : +z axis, 3 : -x axis, 4 : -y axis, 5 : -z axis
		std::array<std::pair<int, float>, 6> pairs{ std::pair{0, axis_x.w}, {1, axis_y.w}, {2, axis_z.w}, {3, -axis_x.w}, {4, -axis_y.w}, {5, -axis_z.w} };
		std::sort(pairs.begin(), pairs.end(), [](const auto& aA, const auto& aB) { return aA.second > aB.second; });
		int selection = -1;
		for (auto it = pairs.crbegin(); it != pairs.crend() && selection == -1 && interactive; ++it) {
			switch (it->first) {
			case 0: // +x axis
				if (details::is_hit_circle(center + glm::vec2(axis_x), positive_radius, mouse_pos)) 
					selection = 0;
				break;
			case 1: // +y axis
				if (details::is_hit_circle(center + glm::vec2(axis_y), positive_radius, mouse_pos))
					selection = 1;
				break;
			case 2: // +z axis
				if (details::is_hit_circle(center + glm::vec2(axis_z), positive_radius, mouse_pos))
					selection = 2;
				break;
			case 3: // -x axis
				if (details::is_hit_circle(center - glm::vec2(axis_x), negative_radius, mouse_pos))
					selection = 3;
				break;
			case 4: // -y axis
				if (details::is_hit_circle(center - glm::vec2(axis_y), negative_radius, mouse_pos))
					selection = 4;
				break;
			case 5: // -z axis
				if (details::is_hit_circle(center - glm::vec2(axis_z), negative_radius, mouse_pos))
					selection = 5;
				break;
			default: break;
			}
		}
		const bool positive_closer_x = 0.0f >= axis_x.w;
		const bool positive_closer_y = 0.0f >= axis_y.w;
		const bool positive_closer_z = 0.0f >= axis_z.w;

		// draw back first
		const float line_thickness = size * style.line_thickness_scale;
		for (const auto& [fst, snd] : pairs) {
			switch (fst) {
			case 0: // +x axis
				details::draw_positive_line(center, axis_x, positive_closer_x ? style.circle_front_color_x : style.circle_back_color_x, positive_radius, line_thickness, "X", selection == 0);
				continue;
			case 1: // +y axis
				details::draw_positive_line(center, axis_y, positive_closer_y ? style.circle_front_color_y : style.circle_back_color_y, positive_radius, line_thickness, "Y", selection == 1);
				continue;
			case 2: // +z axis
				details::draw_positive_line(center, axis_z, positive_closer_z ? style.circle_front_color_z : style.circle_back_color_z, positive_radius, line_thickness, "Z", selection == 2);
				continue;
			case 3: // -x axis
				details::draw_negative_line(center, axis_x, !positive_closer_x ? style.circle_front_color_x : style.circle_back_color_x, negative_radius, selection == 3);
				continue;
			case 4: // -y axis
				details::draw_negative_line(center, axis_y, !positive_closer_y ? style.circle_front_color_y : style.circle_back_color_y, negative_radius, selection == 4);
				continue;
			case 5: // -z axis
				details::draw_negative_line(center, axis_z, !positive_closer_z ? style.circle_front_color_z : style.circle_back_color_z, negative_radius, selection == 5);
				continue;
			default: break;
			}
		}

		details::config.mDrawList = nullptr;
		return true;
	}
}