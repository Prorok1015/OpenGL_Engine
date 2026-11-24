#include "edt_mesh_gizmo.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace edt::mesh
{
	namespace
	{
		constexpr int k_ring_samples = 48;
		constexpr float k_min_factor = 1e-3f;
		constexpr float k_parallel = 1e-4f;

		glm::vec3 axis_of(const pivot_frame& frame, gizmo_part part)
		{
			switch (part) {
			case gizmo_part::x:
			case gizmo_part::yz:
				return frame.x;
			case gizmo_part::y:
			case gizmo_part::zx:
				return frame.y;
			default:
				return frame.z;
			}
		}

		// The two axes that span a plane handle.
		std::array<glm::vec3, 2> plane_axes(const pivot_frame& frame, gizmo_part part)
		{
			switch (part) {
			case gizmo_part::yz:
				return { frame.y, frame.z };
			case gizmo_part::zx:
				return { frame.z, frame.x };
			default:
				return { frame.x, frame.y };
			}
		}

		float visibility_of_axis(const glm::vec3& axis, const glm::vec3& view)
		{
			const float facing = std::abs(glm::dot(axis, view));
			return 1.f - glm::clamp((facing - k_gizmo_fade_start) / (k_gizmo_fade_end - k_gizmo_fade_start), 0.f, 1.f);
		}

		// A plane handle is flat to the camera when its normal is square to the view.
		float visibility_of_plane(const glm::vec3& normal, const glm::vec3& view)
		{
			const float facing = std::abs(glm::dot(normal, view));
			return glm::clamp((facing - (1.f - k_gizmo_fade_end)) / (k_gizmo_fade_end - k_gizmo_fade_start), 0.f, 1.f);
		}

		float distance_to_segment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
		{
			const glm::vec2 ab = b - a;
			const float length_sq = glm::dot(ab, ab);
			if (!(length_sq > 0.f)) return glm::length(p - a);
			const float t = glm::clamp(glm::dot(p - a, ab) / length_sq, 0.f, 1.f);
			return glm::length(p - (a + ab * t));
		}

		float distance_to_polyline(const glm::vec2& p, const std::vector<glm::vec2>& points)
		{
			float best = std::numeric_limits<float>::max();
			for (std::size_t i = 0; i < points.size(); ++i) {
				best = std::min(best, distance_to_segment(p, points[i], points[(i + 1) % points.size()]));
			}
			return best;
		}

		bool inside_quad(const glm::vec2& p, const std::vector<glm::vec2>& quad)
		{
			bool inside = false;
			for (std::size_t i = 0, j = quad.size() - 1; i < quad.size(); j = i++) {
				const bool crosses = (quad[i].y > p.y) != (quad[j].y > p.y);
				if (crosses && p.x < (quad[j].x - quad[i].x) * (p.y - quad[i].y) / (quad[j].y - quad[i].y) + quad[i].x) inside = !inside;
			}
			return inside;
		}

		std::optional<glm::vec3> ray_plane(const ds::ray3& ray, const glm::vec3& point, const glm::vec3& normal)
		{
			const float denominator = glm::dot(normal, ray.direction);
			if (!(std::abs(denominator) > k_parallel)) return std::nullopt;
			const float t = glm::dot(normal, point - ray.origin) / denominator;
			if (!std::isfinite(t)) return std::nullopt;
			return ray.at(t);
		}

		// The parameter along the line (origin, direction) of the point nearest the ray; nullopt when the two are parallel.
		std::optional<float> line_param_nearest_ray(const glm::vec3& origin, const glm::vec3& direction, const ds::ray3& ray)
		{
			const glm::vec3 w = origin - ray.origin;
			const float b = glm::dot(direction, ray.direction);
			const float denominator = 1.f - b * b;
			if (!(denominator > k_parallel)) return std::nullopt;
			const float d = glm::dot(direction, w);
			const float e = glm::dot(ray.direction, w);
			const float t = (b * e - d) / denominator;
			return std::isfinite(t) ? std::optional<float>(t) : std::nullopt;
		}

		float signed_angle(const glm::vec3& from, const glm::vec3& to, const glm::vec3& axis)
		{
			return std::atan2(glm::dot(axis, glm::cross(from, to)), glm::dot(from, to));
		}

		float snapped(float value, float step)
		{
			return step > 0.f ? snap_to_step(value, step) : value;
		}

		glm::vec3 view_direction_of(const screen_view& screen, const glm::vec2& pixel)
		{
			const auto ray = screen.ray_through(pixel);
			return ray ? ray->direction : glm::vec3(0.f, 0.f, -1.f);
		}
	}

	const gizmo_shape* gizmo_layout::find(gizmo_part part) const
	{
		const auto it = std::ranges::find_if(shapes, [part](const gizmo_shape& shape) { return shape.part == part; });
		return it == shapes.end() ? nullptr : &*it;
	}

	std::optional<gizmo_layout> make_gizmo_layout(const screen_view& screen, const pivot_frame& frame, gizmo_tool tool, const gizmo_metrics& metrics)
	{
		const auto origin = screen.project(frame.origin);
		if (!origin) return std::nullopt;
		const auto here = screen.ray_through(*origin);
		const auto next = screen.ray_through(*origin + glm::vec2(1.f, 0.f));
		if (!here || !next) return std::nullopt;
		const float along = glm::dot(frame.origin - here->origin, here->direction);
		const float world_per_pixel = glm::length(here->at(along) - next->at(along));
		if (!(world_per_pixel > 0.f) || !std::isfinite(world_per_pixel)) return std::nullopt;

		gizmo_layout layout;
		layout.tool = tool;
		layout.frame = frame;
		layout.origin = *origin;
		layout.world_per_pixel = world_per_pixel;
		layout.view_direction = here->direction;
		layout.screen_ring_radius = metrics.arm * 1.25f;
		const float arm = metrics.arm * world_per_pixel;

		const std::array<std::pair<gizmo_part, glm::vec3>, 3> axes{ { { gizmo_part::x, frame.x }, { gizmo_part::y, frame.y }, { gizmo_part::z, frame.z } } };
		const auto pixel_of = [&screen](const glm::vec3& world) { return screen.project(world); };

		for (const auto& [part, axis] : axes) {
			gizmo_shape shape;
			shape.part = part;
			shape.visibility = visibility_of_axis(axis, layout.view_direction);
			if (tool == gizmo_tool::rotate) {
				const glm::vec3 u = std::abs(axis.y) < 0.9f ? glm::normalize(glm::cross(axis, glm::vec3(0.f, 1.f, 0.f))) : glm::normalize(glm::cross(axis, glm::vec3(1.f, 0.f, 0.f)));
				const glm::vec3 v = glm::cross(axis, u);
				bool complete = true;
				for (int i = 0; i < k_ring_samples; ++i) {
					const float angle = 2.f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(k_ring_samples);
					const auto pixel = pixel_of(frame.origin + (u * std::cos(angle) + v * std::sin(angle)) * arm);
					if (!pixel) {
						complete = false;
						break;
					}
					shape.points.push_back(*pixel);
				}
				if (!complete) shape.points.clear();
			} else {
				const auto end = pixel_of(frame.origin + axis * arm);
				if (end) shape.points = { *origin, *end };
				shape.anchor = end.value_or(*origin);
			}
			layout.shapes.push_back(std::move(shape));
		}

		if (tool == gizmo_tool::move) {
			const std::array<gizmo_part, 3> planes{ gizmo_part::yz, gizmo_part::zx, gizmo_part::xy };
			for (gizmo_part part : planes) {
				gizmo_shape shape;
				shape.part = part;
				const auto spanned = plane_axes(frame, part);
				shape.visibility = visibility_of_plane(axis_of(frame, part), layout.view_direction);
				const glm::vec3& a = spanned[0];
				const glm::vec3& b = spanned[1];
				const std::array<glm::vec3, 4> corners{ frame.origin + (a + b) * (arm * metrics.plane_near), frame.origin + (a * metrics.plane_far + b * metrics.plane_near) * arm,
					frame.origin + (a + b) * (arm * metrics.plane_far), frame.origin + (a * metrics.plane_near + b * metrics.plane_far) * arm };
				bool complete = true;
				for (const glm::vec3& corner : corners) {
					const auto pixel = pixel_of(corner);
					if (!pixel) {
						complete = false;
						break;
					}
					shape.points.push_back(*pixel);
				}
				if (!complete) shape.points.clear();
				layout.shapes.push_back(std::move(shape));
			}
		}

		if (tool != gizmo_tool::rotate) {
			gizmo_shape centre;
			centre.part = gizmo_part::centre;
			centre.anchor = *origin;
			layout.shapes.push_back(std::move(centre));
		} else {
			gizmo_shape ring;
			ring.part = gizmo_part::screen;
			ring.anchor = *origin;
			layout.shapes.push_back(std::move(ring));
		}
		return layout;
	}

	gizmo_part pick_gizmo_part(const gizmo_layout& layout, const glm::vec2& mouse, const gizmo_metrics& metrics)
	{
		gizmo_part best = gizmo_part::none;
		float best_distance = std::numeric_limits<float>::max();
		const auto consider = [&](gizmo_part part, float distance) {
			if (distance < best_distance) {
				best = part;
				best_distance = distance;
			}
		};

		const float to_origin = glm::length(mouse - layout.origin);
		if (layout.tool != gizmo_tool::rotate) {
			if (to_origin <= metrics.centre) return gizmo_part::centre;
		}
		for (const gizmo_shape& shape : layout.shapes) {
			if (shape.visibility < 0.1f) continue;
			switch (shape.part) {
			case gizmo_part::x:
			case gizmo_part::y:
			case gizmo_part::z:
				if (shape.points.empty()) break;
				if (layout.tool == gizmo_tool::rotate) {
					const float distance = distance_to_polyline(mouse, shape.points);
					if (distance <= metrics.pick) consider(shape.part, distance);
				} else {
					float distance = distance_to_segment(mouse, shape.points[0], shape.points[1]);
					if (layout.tool == gizmo_tool::scale) distance = std::min(distance, std::max(0.f, glm::length(mouse - shape.anchor) - metrics.cube));
					if (distance <= metrics.pick) consider(shape.part, distance);
				}
				break;
			case gizmo_part::yz:
			case gizmo_part::zx:
			case gizmo_part::xy:
				if (shape.points.size() == 4 && inside_quad(mouse, shape.points)) consider(shape.part, 0.f);
				break;
			case gizmo_part::screen:
				{
					const float distance = std::abs(to_origin - layout.screen_ring_radius);
					if (distance <= metrics.pick) consider(gizmo_part::screen, distance + 0.01f);
				}
				break;
			default:
				break;
			}
		}
		return best;
	}

	std::optional<gizmo_drag> gizmo_drag::begin(const screen_view& screen, const gizmo_layout& layout, gizmo_part part, const glm::vec2& mouse)
	{
		if (part == gizmo_part::none) return std::nullopt;
		const auto ray = screen.ray_through(mouse);
		if (!ray) return std::nullopt;
		gizmo_drag drag;
		drag.m_tool = layout.tool;
		drag.m_part = part;
		drag.m_frame = layout.frame;
		drag.m_view_direction = layout.view_direction;
		drag.m_origin_pixel = layout.origin;
		drag.m_press = mouse;
		const glm::vec3 origin = layout.frame.origin;

		const bool on_axis = part == gizmo_part::x || part == gizmo_part::y || part == gizmo_part::z;
		if (layout.tool == gizmo_tool::scale) {
			if (glm::length(mouse - layout.origin) < 1.f) return std::nullopt;
			return drag;
		}
		if (layout.tool == gizmo_tool::move && on_axis) {
			const auto t = line_param_nearest_ray(origin, axis_of(layout.frame, part), *ray);
			if (!t) return std::nullopt;
			drag.m_start_param = *t;
			return drag;
		}
		glm::vec3 normal = layout.view_direction;
		if (layout.tool == gizmo_tool::move && (part == gizmo_part::yz || part == gizmo_part::zx || part == gizmo_part::xy)) normal = axis_of(layout.frame, part);
		if (layout.tool == gizmo_tool::rotate && on_axis) normal = axis_of(layout.frame, part);
		const auto hit = ray_plane(*ray, origin, normal);
		if (!hit) return std::nullopt;
		drag.m_start_point = *hit;
		return drag;
	}

	std::optional<gizmo_delta> gizmo_drag::update(const screen_view& screen, const glm::vec2& mouse, const gizmo_snap& snap) const
	{
		const auto ray = screen.ray_through(mouse);
		if (!ray) return std::nullopt;
		const glm::vec3 origin = m_frame.origin;
		const bool on_axis = m_part == gizmo_part::x || m_part == gizmo_part::y || m_part == gizmo_part::z;
		gizmo_delta delta;

		if (m_tool == gizmo_tool::move) {
			glm::vec3 translation(0.f);
			if (on_axis) {
				const glm::vec3 axis = axis_of(m_frame, m_part);
				const auto t = line_param_nearest_ray(origin, axis, *ray);
				if (!t) return std::nullopt;
				translation = axis * snapped(*t - m_start_param, snap.move);
			} else {
				const bool plane_handle = m_part == gizmo_part::yz || m_part == gizmo_part::zx || m_part == gizmo_part::xy;
				const glm::vec3 normal = plane_handle ? axis_of(m_frame, m_part) : m_view_direction;
				const auto hit = ray_plane(*ray, origin, normal);
				if (!hit) return std::nullopt;
				const glm::vec3 moved = *hit - m_start_point;
				if (snap.move > 0.f) {
					translation = m_frame.x * snap_to_step(glm::dot(moved, m_frame.x), snap.move) + m_frame.y * snap_to_step(glm::dot(moved, m_frame.y), snap.move)
						+ m_frame.z * snap_to_step(glm::dot(moved, m_frame.z), snap.move);
					if (plane_handle) translation -= normal * glm::dot(translation, normal);
				} else {
					translation = moved;
				}
			}
			delta.translation = translation;
			delta.matrix = translation_matrix(translation);
			return delta;
		}

		if (m_tool == gizmo_tool::rotate) {
			const glm::vec3 axis = on_axis ? axis_of(m_frame, m_part) : m_view_direction;
			const auto hit = ray_plane(*ray, origin, axis);
			if (!hit) return std::nullopt;
			const glm::vec3 from = m_start_point - origin;
			const glm::vec3 to = *hit - origin;
			if (!(glm::length(from) > 1e-6f) || !(glm::length(to) > 1e-6f)) return std::nullopt;
			const glm::vec3 about = on_axis ? axis : -m_view_direction;
			const float angle = snapped(signed_angle(glm::normalize(from), glm::normalize(to), about), snap.rotate_radians);
			delta.angle_radians = angle;
			delta.matrix = rotation_matrix(origin, about, angle);
			return delta;
		}

		const float start = glm::length(m_press - m_origin_pixel);
		const float now = glm::length(mouse - m_origin_pixel);
		const float factor = std::max(snapped(now / start, snap.scale), k_min_factor);
		glm::vec3 factors(1.f);
		if (on_axis) {
			factors[m_part == gizmo_part::x ? 0 : m_part == gizmo_part::y ? 1 : 2] = factor;
		} else {
			factors = glm::vec3(factor);
		}
		delta.factors = factors;
		delta.matrix = scale_matrix(m_frame, factors);
		return delta;
	}

	glm::mat4 numeric_matrix(gizmo_tool tool, const pivot_frame& frame, const glm::vec3& values)
	{
		switch (tool) {
		case gizmo_tool::move:
			return translation_matrix(values - frame.origin);
		case gizmo_tool::rotate:
			{
				const float to_radians = std::numbers::pi_v<float> / 180.f;
				return rotation_matrix(frame.origin, frame.z, values.z * to_radians) * rotation_matrix(frame.origin, frame.y, values.y * to_radians)
					* rotation_matrix(frame.origin, frame.x, values.x * to_radians);
			}
		case gizmo_tool::scale:
			return scale_matrix(frame, values);
		}
		return glm::mat4(1.f);
	}

	glm::vec3 numeric_defaults(gizmo_tool tool, const pivot_frame& frame)
	{
		switch (tool) {
		case gizmo_tool::move:
			return frame.origin;
		case gizmo_tool::rotate:
			return glm::vec3(0.f);
		case gizmo_tool::scale:
			return glm::vec3(1.f);
		}
		return glm::vec3(0.f);
	}
}
