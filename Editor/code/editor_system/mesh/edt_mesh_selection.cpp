#include "edt_mesh_selection.h"
#include <algorithm>

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

	std::vector<vertex_id> vertices_of_face(const face& f)
	{
		std::vector<vertex_id> ids;
		ids.reserve(f.corners.size());
		for (const corner& c : f.corners) ids.push_back(c.vertex);
		return ids;
	}

	std::vector<edge_key> edges_of_face(const face& f)
	{
		std::vector<edge_key> keys;
		keys.reserve(f.corners.size());
		for (std::size_t i = 0; i < f.corners.size(); ++i) {
			keys.push_back(edge_key::of(f.corners[i].vertex, f.corners[(i + 1) % f.corners.size()].vertex));
		}
		return keys;
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
		if (const face* f = mesh.find_face(id)) {
			for (vertex_id v : vertices_of_face(*f)) result.insert(v);
		}
	}
	return result;
}

void mesh_selection::set_mode(select_mode mode, const poly_mesh& mesh, const mesh_adjacency& adjacency)
{
	if (mode == m_mode) return;
	const select_mode from = m_mode;
	std::set<vertex_id> vertices;
	std::set<edge_key> edges;
	std::set<face_id> faces;

	if (from == select_mode::vertex) {
		if (mode == select_mode::edge) {
			for (const edge_key& key : adjacency.edges()) {
				if (m_vertices.contains(key.a) && m_vertices.contains(key.b)) edges.insert(key);
			}
		} else {
			std::set<face_id> candidates;
			for (vertex_id v : m_vertices) {
				for (face_id f : adjacency.faces_of_vertex(v)) candidates.insert(f);
			}
			for (face_id f : candidates) {
				const face* shape = mesh.find_face(f);
				if (!shape) continue;
				const auto edge_set = edges_of_face(*shape);
				const bool covered = std::ranges::all_of(edge_set, [&](const edge_key& key) { return m_vertices.contains(key.a) && m_vertices.contains(key.b); });
				if (covered) faces.insert(f);
			}
		}
	} else if (from == select_mode::edge) {
		if (mode == select_mode::vertex) {
			for (const edge_key& key : m_edges) {
				vertices.insert(key.a);
				vertices.insert(key.b);
			}
		} else {
			std::set<face_id> candidates;
			for (const edge_key& key : m_edges) {
				for (face_id f : adjacency.faces_of_edge(key)) candidates.insert(f);
			}
			for (face_id f : candidates) {
				const face* shape = mesh.find_face(f);
				if (!shape) continue;
				const auto edge_set = edges_of_face(*shape);
				if (std::ranges::all_of(edge_set, [&](const edge_key& key) { return m_edges.contains(key); })) faces.insert(f);
			}
		}
	} else {
		for (face_id f : m_faces) {
			const face* shape = mesh.find_face(f);
			if (!shape) continue;
			for (const edge_key& key : edges_of_face(*shape)) {
				if (mode == select_mode::edge) {
					edges.insert(key);
				} else {
					vertices.insert(key.a);
					vertices.insert(key.b);
				}
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
	clear();
	switch (m_mode) {
	case select_mode::vertex:
		for (const auto& [id, v] : mesh.vertices()) {
			if (!adjacency.faces_of_vertex(id).empty()) m_vertices.insert(id);
		}
		break;
	case select_mode::edge:
		m_edges.insert(adjacency.edges().begin(), adjacency.edges().end());
		break;
	case select_mode::face:
		for (const auto& [id, f] : mesh.faces()) m_faces.insert(id);
		break;
	}
}

void mesh_selection::invert(const poly_mesh& mesh, const mesh_adjacency& adjacency)
{
	const mesh_selection before = *this;
	select_all(mesh, adjacency);
	for (vertex_id id : before.m_vertices) m_vertices.erase(id);
	for (const edge_key& key : before.m_edges) m_edges.erase(key);
	for (face_id id : before.m_faces) m_faces.erase(id);
}

void mesh_selection::prune(const poly_mesh& mesh, const mesh_adjacency& adjacency)
{
	std::erase_if(m_vertices, [&](vertex_id id) { return adjacency.faces_of_vertex(id).empty() || !mesh.find_vertex(id); });
	std::erase_if(m_edges, [&](const edge_key& key) { return adjacency.faces_of_edge(key).empty(); });
	std::erase_if(m_faces, [&](face_id id) { return !mesh.find_face(id); });
}
}
