#pragma once
#include <glm/glm.hpp>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace edt::mesh
{
	using vertex_id = std::uint32_t;
	using face_id = std::uint32_t;
	inline constexpr std::uint32_t invalid_id = 0;
	// Never given out: the id after it would wrap to invalid_id.
	inline constexpr std::uint32_t reserved_id = 0xFFFFFFFFu;

	// Identity of an edge: its two vertices, the smaller id first. Edges are not stored; they follow from the faces.
	struct edge_key
	{
		vertex_id a = invalid_id;
		vertex_id b = invalid_id;

		static edge_key of(vertex_id first, vertex_id second) { return first < second ? edge_key{ first, second } : edge_key{ second, first }; }
		auto operator<=>(const edge_key&) const = default;
	};

	enum class edge_flag : std::uint8_t
	{
		none = 0,
		sharp = 1,
		seam = 2,
	};

	constexpr edge_flag operator|(edge_flag lhs, edge_flag rhs) { return static_cast<edge_flag>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs)); }
	constexpr bool has_flag(edge_flag flags, edge_flag flag) { return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(flag)) != 0; }

	struct vertex
	{
		glm::vec3 position{ 0.f };
	};

	struct corner
	{
		vertex_id vertex = invalid_id;
		glm::vec2 uv{ 0.f };
	};

	struct face
	{
		std::vector<corner> corners;
		std::uint32_t material_slot = 0;
	};

	// Indexed polygon mesh. Ids are never reused and survive the removal of other elements. The maps are ordered by id:
	// every walk over the mesh, the bake included, is deterministic.
	class poly_mesh
	{
	public:
		// invalid_id when the ids ran out (past reserved_id).
		vertex_id add_vertex(const glm::vec3& position);
		// invalid_id when the ids ran out (past reserved_id), fewer than 3 corners, an unknown vertex, or a vertex twice in a row (the last corner counts as the one before the first).
		face_id add_face(std::vector<corner> corners, std::uint32_t material_slot = 0);
		bool remove_face(face_id id);
		// Only a vertex no face uses; false otherwise.
		bool remove_vertex(vertex_id id);
		bool set_position(vertex_id id, const glm::vec3& position);

		// Re-inserts an element under a known id (reading a file, undoing a removal). False when the id is taken, invalid_id or reserved_id.
		bool restore_vertex(vertex_id id, const glm::vec3& position);
		bool restore_face(face_id id, std::vector<corner> corners, std::uint32_t material_slot);

		const vertex* find_vertex(vertex_id id) const;
		const face* find_face(face_id id) const;
		const std::map<vertex_id, vertex>& vertices() const { return m_vertices; }
		const std::map<face_id, face>& faces() const { return m_faces; }

		void set_edge_flags(const edge_key& key, edge_flag flags);
		edge_flag edge_flags(const edge_key& key) const;
		const std::map<edge_key, edge_flag>& flagged_edges() const { return m_edge_flags; }

		// Grows with every change; a cache built from the mesh is valid for the generation it was built at.
		std::uint64_t generation() const { return m_generation; }
		std::size_t triangle_count() const;

		// The next ids both meshes would hand out are the same: two meshes that are equal otherwise but differ here are not the same state.
		bool same_id_counters(const poly_mesh& other) const { return m_next_vertex == other.m_next_vertex && m_next_face == other.m_next_face; }

		// What is wrong with the mesh, one line each: empty when it is sound.
		std::vector<std::string> validate() const;

	private:
		static bool corners_are_sound(const std::map<vertex_id, vertex>& vertices, const std::vector<corner>& corners);

		std::map<vertex_id, vertex> m_vertices;
		std::map<face_id, face> m_faces;
		std::map<edge_key, edge_flag> m_edge_flags;
		vertex_id m_next_vertex = 1;
		face_id m_next_face = 1;
		std::uint64_t m_generation = 0;
	};
}
