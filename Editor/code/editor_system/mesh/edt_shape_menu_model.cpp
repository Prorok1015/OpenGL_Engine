#include "edt_shape_menu_model.h"
#include "edt_mesh_io.h"
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_node_id.h>
#include <cmath>
#include <format>
#include <utility>

namespace json = boost::json;

namespace edt::mesh
{
	namespace
	{
		std::vector<double> numbers_of(const json::value& value)
		{
			std::vector<double> out;
			if (const json::array* array = value.if_array()) {
				for (const json::value& item : *array) out.push_back(item.to_number<double>());
			} else {
				out.push_back(value.to_number<double>());
			}
			return out;
		}

		constexpr double largest_count = 4294967295.0;

		std::string whole_number_problem(double value)
		{
			if (!std::isfinite(value) || value < 0.0 || value > largest_count || value != std::floor(value)) return "a whole number from 0 to 4294967295";
			return {};
		}

		bool is_whole(const json::value& value)
		{
			return value.is_int64() || value.is_uint64();
		}

		std::vector<shape_field> fields_of(const json::object& params)
		{
			std::vector<shape_field> out;
			out.reserve(params.size());
			for (const auto& [key, value] : params) {
				const json::value& first = value.is_array() && !value.as_array().empty() ? value.as_array().front() : value;
				out.push_back({ std::string(key), numbers_of(value), is_whole(first) });
			}
			return out;
		}
	}

	parametric_shape_form::parametric_shape_form()
	{
		set_kind(shape_kinds.front());
	}

	void parametric_shape_form::set_kind(std::string_view kind)
	{
		const std::optional<shape_params> defaults = default_shape_params(kind);
		ASSERT_MSG(defaults, "an unknown shape kind in the Add shape dialog");
		m_kind = std::string(kind);
		m_fields = fields_of(shape_params_to_json(*defaults));
	}

	json::object parametric_shape_form::params_json() const
	{
		json::object out;
		for (const shape_field& field : m_fields) {
			const auto one = [&field](double value) -> json::value {
				if (!field.integer) return json::value(value);
				return json::value(whole_number_problem(value).empty() ? static_cast<std::uint64_t>(std::llround(value)) : std::uint64_t{ 0 });
			};
			if (field.values.size() == 1) {
				out[field.key] = one(field.values.front());
				continue;
			}
			json::array items;
			for (double value : field.values) items.push_back(one(value));
			out[field.key] = std::move(items);
		}
		return out;
	}

	std::string parametric_shape_form::problem() const
	{
		if (name.empty()) return "the name is empty";
		for (const shape_field& field : m_fields) {
			for (const double value : field.values) {
				if (!std::isfinite(value)) return std::format("{} must be a finite number", field.key);
				if (field.integer) {
					if (const std::string needed = whole_number_problem(value); !needed.empty()) return std::format("{} must be {}", field.key, needed);
				}
			}
		}
		std::string error;
		const std::optional<shape_params> params = shape_params_from_json(m_kind, params_json(), error);
		if (!params) return error;
		return make_shape(*params).error;
	}

	json::object parametric_shape_form::create_args() const
	{
		json::object args{ { "name", name }, { "kind", m_kind }, { "params", params_json() } };
		if (!folder.empty()) args["folder"] = folder;
		return args;
	}

	json::object node_ref(std::string_view world, std::span<const std::uint64_t> path)
	{
		json::array ids;
		ids.reserve(path.size());
		for (const std::uint64_t id : path) ids.emplace_back(json::string(desc::format_node_id(id)));
		return json::object{ { "context", std::string(world) }, { "path", std::move(ids) } };
	}

	json::object add_base_args(const json::value& parent, std::string_view shape)
	{
		return json::object{ { "parent", parent }, { "shape", std::string(shape) } };
	}

	json::object add_parametric_args(const json::value& parent, std::string_view poly, std::string_view name)
	{
		return json::object{ { "poly", std::string(poly) }, { "parent", parent }, { "name", std::string(name) } };
	}

	json::object mesh_fields(std::string_view geometry, const material_range& range)
	{
		return json::object{ { "geometry", std::string(geometry) }, { "vx_begin", std::uint64_t{ range.vx_begin } }, { "vx_end", std::uint64_t{ range.vx_end } },
			{ "ind_begin", std::uint64_t{ range.ind_begin } }, { "ind_end", std::uint64_t{ range.ind_end } } };
	}

	std::optional<json::object> mesh_fields_of_created(const json::object& created)
	{
		json::object fields;
		for (const char* key : { "geometry", "vx_begin", "vx_end", "ind_begin", "ind_end" }) {
			const json::value* value = created.if_contains(key);
			if (!value) return std::nullopt;
			fields[key] = *value;
		}
		return fields;
	}

	scn::prefab_desc::prefab_node mesh_node(std::string_view name, const json::object& fields)
	{
		scn::prefab_desc::prefab_node node;
		node.name = std::string(name);
		node.node_id = desc::new_node_id();
		scn::prefab_desc::prefab_comp_node mesh;
		mesh.type_name = "mesh_node_desc";
		mesh.overrides = fields;
		node.components["mesh"] = std::move(mesh);
		return node;
	}

	scn::prefab_desc::prefab_node base_shape_node(const base_shape& shape)
	{
		return mesh_node(shape.name, mesh_fields(base_shape_path(shape), base_shape_range(shape)));
	}
}
