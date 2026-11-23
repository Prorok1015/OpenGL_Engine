#pragma once
#include "edt_mesh_base_shapes.h"
#include "edt_mesh_shapes.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <array>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace edt::mesh
{
	inline constexpr std::array<std::string_view, 5> shape_kinds{ "box", "plane", "sphere", "cylinder", "capsule" };

	// One parameter of a shape in the dialog: a number or a vector of numbers (box size, plane size).
	struct shape_field
	{
		std::string key;
		std::vector<double> values;
		bool integer = false;
	};

	// The "Parametric..." dialog of the Add shape menu without ImGui: the fields of one kind, the name and folder of the files,
	// and the arguments of shape.create they make. The checks are the command's own (`shape_params_from_json`, `make_shape`),
	// so the dialog refuses what the command would refuse.
	class parametric_shape_form
	{
	public:
		parametric_shape_form();

		std::string_view kind() const { return m_kind; }
		// Takes the kind's default parameters; the name and the folder stay.
		void set_kind(std::string_view kind);
		std::vector<shape_field>& fields() { return m_fields; }
		const std::vector<shape_field>& fields() const { return m_fields; }

		std::string name;
		std::string folder;

		// Empty when shape.create would take it; else why not. Bakes the mesh once, so ask on Create, not every frame.
		std::string problem() const;
		// The arguments of shape.create; call after problem() is empty.
		boost::json::object create_args() const;

	private:
		boost::json::object params_json() const;

		std::string m_kind;
		std::vector<shape_field> m_fields;
	};

	// What a person picked in the Add shape menu.
	struct shape_request
	{
		// A ready-made shape of core/res/shapes (shape.add_base); empty for a parametric one.
		std::string base_shape;
		// The arguments of shape.create for a parametric shape; null for a ready-made one.
		boost::json::value create_args;
		// Node ids from the root to the node the shape goes under (empty: the root).
		std::vector<std::uint64_t> parent;
	};

	// A node ref of the scene commands: {context: world name, path: [node ids as the commands spell them]}.
	boost::json::object node_ref(std::string_view world, std::span<const std::uint64_t> path);

	// shape.add_base {parent, shape}
	boost::json::object add_base_args(const boost::json::value& parent, std::string_view shape);
	// shape.add {poly, parent, name}: the node for a shape shape.create made.
	boost::json::object add_parametric_args(const boost::json::value& parent, std::string_view poly, std::string_view name);

	// The fields of a mesh_node_desc for a range of a geometry file, as shape.add and shape.add_base write them.
	boost::json::object mesh_fields(std::string_view geometry, const material_range& range);
	// The same from the answer of shape.create (geometry, vx_begin, vx_end, ind_begin, ind_end). Nullopt when one is missing.
	std::optional<boost::json::object> mesh_fields_of_created(const boost::json::object& created);

	// A prefab node with a mesh: the prefab editor adds it to its working copy, as shape.add_base / shape.add make it in a level.
	scn::prefab_desc::prefab_node mesh_node(std::string_view name, const boost::json::object& fields);
	scn::prefab_desc::prefab_node base_shape_node(const base_shape& shape);
}
