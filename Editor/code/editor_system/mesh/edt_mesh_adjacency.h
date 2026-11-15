#pragma once
#include "edt_mesh_model.h"
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace edt::mesh
{
	// Edge, vertex and face neighbours of a mesh at one generation. A cache: built from the mesh, thrown away on a change.
	class mesh_adjacency
	{
	public:
		explicit mesh_adjacency(const poly_mesh& mesh);

		bool is_current(const poly_mesh& mesh) const { return mesh.generation() == m_generation; }

		// Every edge some face uses, ordered by key.
		std::span<const edge_key> edges() const { return m_edges; }
		std::span<const face_id> faces_of_edge(const edge_key& key) const;
		std::span<const edge_key> edges_of_vertex(vertex_id id) const;
		std::span<const face_id> faces_of_vertex(vertex_id id) const;

		bool is_boundary(const edge_key& key) const { return faces_of_edge(key).size() == 1; }
		bool is_non_manifold(const edge_key& key) const { return faces_of_edge(key).size() > 2; }
		std::size_t non_manifold_edge_count() const;

		// Vertices a face uses, edges and faces: V - E + F of the used part of the mesh (2 for a closed sphere-like surface).
		std::int64_t euler_characteristic() const;
		std::size_t used_vertex_count() const { return m_faces_of_vertex.size(); }

	private:
		std::uint64_t m_generation = 0;
		std::size_t m_face_count = 0;
		std::vector<edge_key> m_edges;
		std::map<edge_key, std::vector<face_id>> m_faces_of_edge;
		std::map<vertex_id, std::vector<edge_key>> m_edges_of_vertex;
		std::map<vertex_id, std::vector<face_id>> m_faces_of_vertex;
	};
}
