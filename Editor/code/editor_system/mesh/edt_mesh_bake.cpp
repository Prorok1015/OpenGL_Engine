#include "edt_mesh_bake.h"
#include "edt_mesh_adjacency.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <tuple>

namespace edt::mesh
{
	namespace
	{
		struct corner_ref
		{
			face_id face = invalid_id;
			std::uint32_t index = 0;
		};

		struct face_info
		{
			glm::vec3 normal{ 0.f };
			bool has_area = false;
			std::vector<std::array<std::uint32_t, 3>> triangles;
		};

		struct out_vertex
		{
			glm::vec3 position{ 0.f };
			glm::vec3 normal{ 0.f };
			glm::vec2 uv{ 0.f };
			glm::vec3 tangent{ 0.f };
			glm::vec3 bitangent{ 0.f };
		};

		glm::vec3 newell_normal(const std::vector<glm::vec3>& points)
		{
			glm::vec3 sum{ 0.f };
			for (std::size_t i = 0; i < points.size(); ++i) {
				const glm::vec3& current = points[i];
				const glm::vec3& next = points[(i + 1) % points.size()];
				sum.x += (current.y - next.y) * (current.z + next.z);
				sum.y += (current.z - next.z) * (current.x + next.x);
				sum.z += (current.x - next.x) * (current.y + next.y);
			}
			return sum;
		}

		glm::vec3 any_perpendicular(const glm::vec3& n)
		{
			const glm::vec3 axis = std::abs(n.x) < 0.9f ? glm::vec3{ 1.f, 0.f, 0.f } : glm::vec3{ 0.f, 1.f, 0.f };
			return glm::normalize(glm::cross(n, axis));
		}

		bool inside_triangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
		{
			const auto side = [](const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& q) {
				return (p1.x - p0.x) * (q.y - p0.y) - (p1.y - p0.y) * (q.x - p0.x);
			};
			return side(a, b, p) > 1e-9f && side(b, c, p) > 1e-9f && side(c, a, p) > 1e-9f;
		}

		std::vector<std::array<std::uint32_t, 3>> triangulate(const std::vector<glm::vec3>& points, const glm::vec3& normal)
		{
			const std::uint32_t count = static_cast<std::uint32_t>(points.size());
			std::vector<std::array<std::uint32_t, 3>> triangles;
			triangles.reserve(count - 2);
			if (count == 3) {
				triangles.push_back({ 0, 1, 2 });
				return triangles;
			}

			const glm::vec3 u_axis = any_perpendicular(normal);
			const glm::vec3 v_axis = glm::cross(normal, u_axis);
			std::vector<glm::vec2> flat;
			flat.reserve(count);
			for (const glm::vec3& p : points) {
				flat.emplace_back(glm::dot(p, u_axis), glm::dot(p, v_axis));
			}
			const auto turn = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
				return (flat[b].x - flat[a].x) * (flat[c].y - flat[b].y) - (flat[b].y - flat[a].y) * (flat[c].x - flat[b].x);
			};

			bool convex = true;
			for (std::uint32_t i = 0; i < count && convex; ++i) {
				convex = turn(i, (i + 1) % count, (i + 2) % count) >= -1e-9f;
			}
			if (convex) {
				for (std::uint32_t i = 1; i + 1 < count; ++i) {
					triangles.push_back({ 0, i, i + 1 });
				}
				return triangles;
			}

			std::vector<std::uint32_t> rest(count);
			std::iota(rest.begin(), rest.end(), 0u);
			while (rest.size() > 3) {
				std::size_t ear = 0;
				bool found = false;
				for (std::size_t i = 0; i < rest.size() && !found; ++i) {
					const std::uint32_t prev = rest[(i + rest.size() - 1) % rest.size()];
					const std::uint32_t curr = rest[i];
					const std::uint32_t next = rest[(i + 1) % rest.size()];
					if (turn(prev, curr, next) <= 0.f) {
						continue;
					}
					const bool empty = std::ranges::none_of(rest, [&](std::uint32_t other) {
						return other != prev && other != curr && other != next && inside_triangle(flat[other], flat[prev], flat[curr], flat[next]);
					});
					if (empty) {
						ear = i;
						found = true;
					}
				}
				const std::uint32_t prev = rest[(ear + rest.size() - 1) % rest.size()];
				const std::uint32_t next = rest[(ear + 1) % rest.size()];
				triangles.push_back({ prev, rest[ear], next });
				rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(ear));
			}
			triangles.push_back({ rest[0], rest[1], rest[2] });
			return triangles;
		}

		// Union-find over the corners that meet at one vertex.
		class corner_groups
		{
		public:
			explicit corner_groups(std::size_t size) : m_parent(size)
			{
				std::iota(m_parent.begin(), m_parent.end(), std::size_t{ 0 });
			}

			std::size_t find(std::size_t item)
			{
				while (m_parent[item] != item) {
					m_parent[item] = m_parent[m_parent[item]];
					item = m_parent[item];
				}
				return item;
			}

			void join(std::size_t first, std::size_t second)
			{
				const std::size_t a = find(first);
				const std::size_t b = find(second);
				if (a != b) {
					m_parent[std::max(a, b)] = std::min(a, b);
				}
			}

		private:
			std::vector<std::size_t> m_parent;
		};

		vertex_id neighbour(const face& f, std::uint32_t index, bool next)
		{
			const std::size_t count = f.corners.size();
			return f.corners[next ? (index + 1) % count : (index + count - 1) % count].vertex;
		}

		float corner_angle(const poly_mesh& mesh, const face& f, std::uint32_t index)
		{
			const glm::vec3& here = mesh.find_vertex(f.corners[index].vertex)->position;
			const glm::vec3 to_prev = mesh.find_vertex(neighbour(f, index, false))->position - here;
			const glm::vec3 to_next = mesh.find_vertex(neighbour(f, index, true))->position - here;
			const float lengths = glm::length(to_prev) * glm::length(to_next);
			if (lengths <= 0.f) {
				return 0.f;
			}
			return std::acos(std::clamp(glm::dot(to_prev, to_next) / lengths, -1.f, 1.f));
		}

		using vertex_key = std::tuple<vertex_id, std::size_t, float, float>;
	}

	baked_mesh bake(const poly_mesh& mesh, const bake_settings& settings)
	{
		baked_mesh result;
		const mesh_adjacency adjacency(mesh);
		const float min_dot = std::cos(settings.smooth_angle_degrees * std::numbers::pi_v<float> / 180.f);

		std::map<face_id, face_info> infos;
		for (const auto& [id, f] : mesh.faces()) {
			std::vector<glm::vec3> points;
			points.reserve(f.corners.size());
			for (const corner& c : f.corners) {
				points.push_back(mesh.find_vertex(c.vertex)->position);
			}
			const glm::vec3 area_normal = newell_normal(points);
			face_info& info = infos[id];
			if (glm::length(area_normal) < 1e-12f) {
				result.skipped_faces.push_back(id);
				continue;
			}
			info.has_area = true;
			info.normal = glm::normalize(area_normal);
			info.triangles = triangulate(points, info.normal);
		}

		std::map<vertex_id, std::vector<corner_ref>> corners_at;
		for (const auto& [id, f] : mesh.faces()) {
			if (!infos[id].has_area) {
				continue;
			}
			for (std::uint32_t i = 0; i < f.corners.size(); ++i) {
				corners_at[f.corners[i].vertex].push_back({ id, i });
			}
		}

		// Smoothing group of every corner: its root inside the corners of its vertex, and the group's normal.
		std::map<std::pair<face_id, std::uint32_t>, std::size_t> group_of;
		std::map<std::pair<vertex_id, std::size_t>, glm::vec3> group_normal;
		for (const auto& [vertex_value, refs] : corners_at) {
			corner_groups groups(refs.size());
			for (std::size_t p = 0; p < refs.size(); ++p) {
				const face& f = *mesh.find_face(refs[p].face);
				for (const bool next : { false, true }) {
					const vertex_id other_end = neighbour(f, refs[p].index, next);
					const edge_key key = edge_key::of(vertex_value, other_end);
					const auto sharing = adjacency.faces_of_edge(key);
					if (sharing.size() != 2 || has_flag(mesh.edge_flags(key), edge_flag::sharp)) {
						continue;
					}
					for (std::size_t q = 0; q < refs.size(); ++q) {
						if (refs[q].face == refs[p].face || (refs[q].face != sharing[0] && refs[q].face != sharing[1])) {
							continue;
						}
						const face& g = *mesh.find_face(refs[q].face);
						const bool touches = neighbour(g, refs[q].index, false) == other_end || neighbour(g, refs[q].index, true) == other_end;
						if (touches && glm::dot(infos[refs[p].face].normal, infos[refs[q].face].normal) >= min_dot) {
							groups.join(p, q);
						}
					}
				}
			}
			for (std::size_t p = 0; p < refs.size(); ++p) {
				const std::size_t root = groups.find(p);
				group_of[{ refs[p].face, refs[p].index }] = root;
				const face& f = *mesh.find_face(refs[p].face);
				group_normal[{ vertex_value, root }] += infos[refs[p].face].normal * corner_angle(mesh, f, refs[p].index);
			}
		}

		std::set<std::uint32_t> slots;
		for (const auto& [id, f] : mesh.faces()) {
			if (infos[id].has_area) {
				slots.insert(f.material_slot);
			}
		}

		std::vector<out_vertex> vertices;
		std::vector<std::uint32_t> indices;
		for (const std::uint32_t slot : slots) {
			material_range range;
			range.slot = slot;
			range.vx_begin = static_cast<std::uint32_t>(vertices.size());
			range.ind_begin = static_cast<std::uint32_t>(indices.size());
			std::map<vertex_key, std::uint32_t> emitted;

			for (const auto& [id, f] : mesh.faces()) {
				const face_info& info = infos[id];
				if (f.material_slot != slot || !info.has_area) {
					continue;
				}
				std::vector<std::uint32_t> local(f.corners.size());
				for (std::uint32_t i = 0; i < f.corners.size(); ++i) {
					const corner& c = f.corners[i];
					const std::size_t root = group_of[{ id, i }];
					const vertex_key key{ c.vertex, root, c.uv.x, c.uv.y };
					const auto [it, added] = emitted.try_emplace(key, static_cast<std::uint32_t>(vertices.size()) - range.vx_begin);
					if (added) {
						glm::vec3 normal = group_normal[{ c.vertex, root }];
						normal = glm::length(normal) < 1e-12f ? info.normal : glm::normalize(normal);
						vertices.push_back({ mesh.find_vertex(c.vertex)->position, normal, c.uv, glm::vec3{ 0.f }, glm::vec3{ 0.f } });
					}
					local[i] = it->second;
				}
				for (const auto& tri : info.triangles) {
					out_vertex& v0 = vertices[range.vx_begin + local[tri[0]]];
					out_vertex& v1 = vertices[range.vx_begin + local[tri[1]]];
					out_vertex& v2 = vertices[range.vx_begin + local[tri[2]]];
					const glm::vec3 e1 = v1.position - v0.position;
					const glm::vec3 e2 = v2.position - v0.position;
					const glm::vec2 d1 = v1.uv - v0.uv;
					const glm::vec2 d2 = v2.uv - v0.uv;
					const float det = d1.x * d2.y - d2.x * d1.y;
					if (std::abs(det) > 1e-12f) {
						const glm::vec3 tangent = (e1 * d2.y - e2 * d1.y) / det;
						const glm::vec3 bitangent = (e2 * d1.x - e1 * d2.x) / det;
						for (out_vertex* v : { &v0, &v1, &v2 }) {
							v->tangent += tangent;
							v->bitangent += bitangent;
						}
					}
					for (const std::uint32_t corner_index : tri) {
						indices.push_back(local[corner_index]);
					}
				}
			}
			range.vx_end = static_cast<std::uint32_t>(vertices.size());
			range.ind_end = static_cast<std::uint32_t>(indices.size());
			result.ranges.push_back(range);
		}

		for (out_vertex& v : vertices) {
			glm::vec3 tangent = v.tangent - v.normal * glm::dot(v.normal, v.tangent);
			tangent = glm::length(tangent) < 1e-8f ? any_perpendicular(v.normal) : glm::normalize(tangent);
			glm::vec3 bitangent = glm::cross(v.normal, tangent);
			if (glm::dot(bitangent, v.bitangent) < 0.f) {
				bitangent = -bitangent;
			}
			v.tangent = tangent;
			v.bitangent = bitangent;
		}

		using rnd::driver::SHADER_DATA_TYPE;
		result.geometry.layout = rnd::driver::BufferLayout{
			{ SHADER_DATA_TYPE::VEC3_F, "position" }, { SHADER_DATA_TYPE::VEC3_F, "normal" }, { SHADER_DATA_TYPE::VEC2_F, "uv" },
			{ SHADER_DATA_TYPE::VEC3_F, "tangent" }, { SHADER_DATA_TYPE::VEC3_F, "bitangent" } };
		result.geometry.indices = std::move(indices);

		constexpr std::size_t floats_per_vertex = 14;
		result.geometry.vertices.resize(vertices.size() * floats_per_vertex * sizeof(float));
		std::byte* out = result.geometry.vertices.data();
		for (const out_vertex& v : vertices) {
			const float packed[floats_per_vertex] = { v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y,
				v.tangent.x, v.tangent.y, v.tangent.z, v.bitangent.x, v.bitangent.y, v.bitangent.z };
			std::memcpy(out, packed, sizeof(packed));
			out += sizeof(packed);
		}
		return result;
	}
}
