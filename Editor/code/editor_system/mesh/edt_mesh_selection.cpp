#include "edt_mesh_selection.h"
#include <snk/common/engine_assert.h>
#include <algorithm>
#include <utility>
#include <vector>

namespace edt::mesh
{
	namespace
	{
		template <class Set, class Key>
		void apply(Set& set, const Key& key, select_op op)
		{
			switch (op) {
			case select_op::replace:
				set.clear();
				set.insert(key);
				break;
			case select_op::add:
				set.insert(key);
				break;
			case select_op::toggle:
				if (!set.erase(key)) set.insert(key);
				break;
			}
		}

		template <class Fn>
		void for_each_edge_of_face(const face& shape, Fn&& fn)
		{
			const std::size_t count = shape.corners.size();
			for (std::size_t i = 0; i < count; ++i) {
				fn(edge_key::of(shape.corners[i].vertex, shape.corners[(i + 1) % count].vertex));
			}
		}

		bool covers_every_edge(const face& shape, const std::set<edge_key>& edges)
		{
			bool covered = true;
			for_each_edge_of_face(shape, [&](const edge_key& key) { covered = covered && edges.contains(key); });
			return covered;
		}
	}

	void mesh_selection::clear()
	{
		m_vertices.clear();
		m_edges.clear();
		m_faces.clear();
	}

	void mesh_selection::apply_vertex(vertex_id id, select_op op)
	{
		if (m_mode == select_mode::vertex) apply(m_vertices, id, op);
	}

	void mesh_selection::apply_edge(const edge_key& key, select_op op)
	{
		if (m_mode == select_mode::edge) apply(m_edges, key, op);
	}

	void mesh_selection::apply_face(face_id id, select_op op)
	{
		if (m_mode == select_mode::face) apply(m_faces, id, op);
	}

	std::set<vertex_id> mesh_selection::touched_vertices(const poly_mesh& mesh) const
	{
		std::set<vertex_id> result = m_vertices;
		for (const edge_key& key : m_edges) {
			result.insert(key.a);
			result.insert(key.b);
		}
		for (face_id id : m_faces) {
			if (const face* shape = mesh.find_face(id)) {
				for (const corner& c : shape->corners) result.insert(c.vertex);
			}
		}
		return result;
	}

	void mesh_selection::set_mode(select_mode mode, const poly_mesh& mesh, const mesh_adjacency& adjacency)
	{
		ASSERT_MSG(adjacency.is_current(mesh), "the adjacency is older than the mesh");
		if (mode == m_mode) return;
		std::set<vertex_id> vertices;
		std::set<edge_key> edges;
		std::set<face_id> faces;

		if (m_mode == select_mode::vertex) {
			if (mode == select_mode::edge) {
				for (vertex_id v : m_vertices) {
					for (const edge_key& key : adjacency.edges_of_vertex(v)) {
						if (m_vertices.contains(key.a) && m_vertices.contains(key.b)) edges.insert(key);
					}
				}
			} else {
				for (vertex_id v : m_vertices) {
					for (face_id id : adjacency.faces_of_vertex(v)) {
						const face* shape = mesh.find_face(id);
						const bool covered = shape && std::ranges::all_of(shape->corners, [&](const corner& c) { return m_vertices.contains(c.vertex); });
						if (covered) faces.insert(id);
					}
				}
			}
		} else if (m_mode == select_mode::edge) {
			if (mode == select_mode::vertex) {
				for (const edge_key& key : m_edges) {
					vertices.insert(key.a);
					vertices.insert(key.b);
				}
			} else {
				for (const edge_key& key : m_edges) {
					for (face_id id : adjacency.faces_of_edge(key)) {
						const face* shape = mesh.find_face(id);
						if (shape && covers_every_edge(*shape, m_edges)) faces.insert(id);
					}
				}
			}
		} else {
			for (face_id id : m_faces) {
				const face* shape = mesh.find_face(id);
				if (!shape) continue;
				if (mode == select_mode::edge) {
					for_each_edge_of_face(*shape, [&](const edge_key& key) { edges.insert(key); });
				} else {
					for (const corner& c : shape->corners) vertices.insert(c.vertex);
				}
			}
		}

		m_mode = mode;
		m_vertices = std::move(vertices);
		m_edges = std::move(edges);
		m_faces = std::move(faces);
	}

	void mesh_selection::select_all(const poly_mesh& mesh, const mesh_adjacency& adjacency)
	{
		ASSERT_MSG(adjacency.is_current(mesh), "the adjacency is older than the mesh");
		clear();
		switch (m_mode) {
		case select_mode::vertex:
			for (const auto& [id, position] : mesh.vertices()) {
				if (!adjacency.faces_of_vertex(id).empty()) m_vertices.insert(id);
			}
			break;
		case select_mode::edge:
			m_edges.insert(adjacency.edges().begin(), adjacency.edges().end());
			break;
		case select_mode::face:
			for (const auto& [id, shape] : mesh.faces()) m_faces.insert(id);
			break;
		}
	}

	void mesh_selection::invert(const poly_mesh& mesh, const mesh_adjacency& adjacency)
	{
		ASSERT_MSG(adjacency.is_current(mesh), "the adjacency is older than the mesh");
		const mesh_selection before = *this;
		select_all(mesh, adjacency);
		for (vertex_id id : before.m_vertices) m_vertices.erase(id);
		for (const edge_key& key : before.m_edges) m_edges.erase(key);
		for (face_id id : before.m_faces) m_faces.erase(id);
	}

	void mesh_selection::prune(const poly_mesh& mesh, const mesh_adjacency& adjacency)
	{
		ASSERT_MSG(adjacency.is_current(mesh), "the adjacency is older than the mesh");
		std::erase_if(m_vertices, [&](vertex_id id) { return adjacency.faces_of_vertex(id).empty(); });
		std::erase_if(m_edges, [&](const edge_key& key) { return adjacency.faces_of_edge(key).empty(); });
		std::erase_if(m_faces, [&](face_id id) { return !mesh.find_face(id); });
	}
}
