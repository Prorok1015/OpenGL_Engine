#pragma once
#include "edt_mesh_adjacency.h"
#include "edt_mesh_model.h"
#include <cstddef>
#include <cstdint>
#include <set>

namespace edt::mesh
{
	enum class select_mode : std::uint8_t
	{
		vertex,
		edge,
		face,
	};

	enum class select_op : std::uint8_t
	{
		replace,
		add,
		toggle,
	};

	// What is selected in a mesh, in one mode: only the set of the current mode is ever filled.
	// Ids are the mesh's own, so a selection outlives edits that keep them; prune() drops the rest.
	class mesh_selection
	{
	public:
		select_mode mode() const { return m_mode; }
		// Carries the selection over: to a finer mode what is touched, to a coarser one what is fully covered.
		void set_mode(select_mode mode, const poly_mesh& mesh, const mesh_adjacency& adjacency);

		const std::set<vertex_id>& vertices() const { return m_vertices; }
		const std::set<edge_key>& edges() const { return m_edges; }
		const std::set<face_id>& faces() const { return m_faces; }

		bool empty() const { return m_vertices.empty() && m_edges.empty() && m_faces.empty(); }
		std::size_t size() const { return m_vertices.size() + m_edges.size() + m_faces.size(); }
		void clear();

		// Each is a no-op for the wrong mode: the caller picks the element kind of the current mode.
		void apply_vertex(vertex_id id, select_op op);
		void apply_edge(const edge_key& key, select_op op);
		void apply_face(face_id id, select_op op);

		void select_all(const poly_mesh& mesh, const mesh_adjacency& adjacency);
		void invert(const poly_mesh& mesh, const mesh_adjacency& adjacency);
		// Drops what the mesh no longer has.
		void prune(const poly_mesh& mesh, const mesh_adjacency& adjacency);

		// Every vertex the selection touches, ordered by id: the pivot and the gizmo work on these.
		std::set<vertex_id> touched_vertices(const poly_mesh& mesh) const;

	private:
		select_mode m_mode = select_mode::vertex;
		std::set<vertex_id> m_vertices;
		std::set<edge_key> m_edges;
		std::set<face_id> m_faces;
	};
}
