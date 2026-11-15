#include "edt_mesh_adjacency.h"
#include <algorithm>

namespace edt::mesh
{
	namespace
	{
		template<class Container, class Key>
		std::span<const typename Container::mapped_type::value_type> lookup(const Container& map, const Key& key)
		{
			const auto it = map.find(key);
			return it == map.end() ? std::span<const typename Container::mapped_type::value_type>{} : std::span{ it->second };
		}

		template<class T>
		void add_unique(std::vector<T>& list, const T& value)
		{
			if (std::ranges::find(list, value) == list.end()) {
				list.push_back(value);
			}
		}
	}

	mesh_adjacency::mesh_adjacency(const poly_mesh& mesh)
		: m_generation(mesh.generation()), m_face_count(mesh.faces().size())
	{
		for (const auto& [id, face_data] : mesh.faces()) {
			const auto& corners = face_data.corners;
			for (std::size_t i = 0; i < corners.size(); ++i) {
				const vertex_id from = corners[i].vertex;
				const vertex_id to = corners[(i + 1) % corners.size()].vertex;
				const edge_key key = edge_key::of(from, to);
				add_unique(m_faces_of_edge[key], id);
				add_unique(m_edges_of_vertex[from], key);
				add_unique(m_edges_of_vertex[to], key);
				add_unique(m_faces_of_vertex[from], id);
			}
		}
		m_edges.reserve(m_faces_of_edge.size());
		for (const auto& entry : m_faces_of_edge) {
			m_edges.push_back(entry.first);
		}
	}

	std::span<const face_id> mesh_adjacency::faces_of_edge(const edge_key& key) const
	{
		return lookup(m_faces_of_edge, key);
	}

	std::span<const edge_key> mesh_adjacency::edges_of_vertex(vertex_id id) const
	{
		return lookup(m_edges_of_vertex, id);
	}

	std::span<const face_id> mesh_adjacency::faces_of_vertex(vertex_id id) const
	{
		return lookup(m_faces_of_vertex, id);
	}

	std::size_t mesh_adjacency::non_manifold_edge_count() const
	{
		return static_cast<std::size_t>(std::ranges::count_if(m_faces_of_edge, [](const auto& entry) { return entry.second.size() > 2; }));
	}

	std::int64_t mesh_adjacency::euler_characteristic() const
	{
		return static_cast<std::int64_t>(m_faces_of_vertex.size()) - static_cast<std::int64_t>(m_edges.size())
			+ static_cast<std::int64_t>(m_face_count);
	}
}
