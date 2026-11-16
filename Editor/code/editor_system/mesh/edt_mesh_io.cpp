#include "edt_mesh_io.h"
#include <snk/desc/desc_system.h>
#include <snk/resource/res_diagnostics.h>
#include <boost/json.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <string_view>

namespace edt::mesh
{
	namespace
	{
		constexpr std::string_view poly_type_name = "poly_mesh_desc";

		[[noreturn]] void fail(std::string_view where, const std::string& reason)
		{
			throw res::load_error(res::diagnostic_kind::load, {}, std::format("{} {}", where, reason));
		}

		json::value float_value(float value)
		{
			char text[32];
			for (int precision = 6; precision <= 9; ++precision) {
				std::snprintf(text, sizeof(text), "%.*g", precision, static_cast<double>(value));
				if (std::strtof(text, nullptr) == value) {
					break;
				}
			}
			return json::value(std::strtod(text, nullptr));
		}

		json::value uint_value(std::uint32_t value)
		{
			return json::value(static_cast<std::uint64_t>(value));
		}

		const json::array& array_of(const json::value& value, std::string_view where)
		{
			if (!value.is_array()) {
				fail(where, "is not an array");
			}
			return value.get_array();
		}

		float float_of(const json::value& value, const std::string& where)
		{
			double number = 0.0;
			if (value.is_double()) {
				number = value.get_double();
			} else if (value.is_int64()) {
				number = static_cast<double>(value.get_int64());
			} else if (value.is_uint64()) {
				number = static_cast<double>(value.get_uint64());
			} else {
				fail(where, "is not a number");
			}
			if (!std::isfinite(number) || std::abs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
				fail(where, "is not a finite float");
			}
			return static_cast<float>(number);
		}

		std::uint32_t uint_of(const json::value& value, const std::string& where)
		{
			if (value.is_uint64() && value.get_uint64() <= std::numeric_limits<std::uint32_t>::max()) {
				return static_cast<std::uint32_t>(value.get_uint64());
			}
			if (value.is_int64() && value.get_int64() >= 0 && value.get_int64() <= std::numeric_limits<std::uint32_t>::max()) {
				return static_cast<std::uint32_t>(value.get_int64());
			}
			fail(where, "is not an integer from 0 to 4294967295");
		}

		const json::array& tuple_of(const json::value& value, std::size_t size, std::string_view where)
		{
			const json::array& items = array_of(value, where);
			if (items.size() != size) {
				fail(where, std::format("has {} items, {} expected", items.size(), size));
			}
			return items;
		}

		const json::value* find(const json::object& object, std::string_view key)
		{
			const auto it = object.find(key);
			return it == object.end() ? nullptr : &it->value();
		}

		float optional_float(const json::object& object, std::string_view key, float fallback, const std::string& where)
		{
			const json::value* value = find(object, key);
			return value ? float_of(*value, std::format("{}.{}", where, key)) : fallback;
		}

		std::uint32_t optional_uint(const json::object& object, std::string_view key, std::uint32_t fallback, const std::string& where)
		{
			const json::value* value = find(object, key);
			return value ? uint_of(*value, std::format("{}.{}", where, key)) : fallback;
		}

		json::array vec_value(std::initializer_list<float> items)
		{
			json::array out;
			for (const float item : items) {
				out.push_back(float_value(item));
			}
			return out;
		}

		json::object params_to_json(const shape_params& params)
		{
			json::object out;
			struct writer
			{
				json::object& out;
				void operator()(const box_params& p) const { out["size"] = vec_value({ p.size.x, p.size.y, p.size.z }); }
				void operator()(const plane_params& p) const
				{
					out["size"] = vec_value({ p.size.x, p.size.y });
					out["segments_x"] = uint_value(p.segments_x);
					out["segments_z"] = uint_value(p.segments_z);
				}
				void operator()(const sphere_params& p) const
				{
					out["radius"] = float_value(p.radius);
					out["segments"] = uint_value(p.segments);
					out["rings"] = uint_value(p.rings);
				}
				void operator()(const cylinder_params& p) const
				{
					out["radius"] = float_value(p.radius);
					out["height"] = float_value(p.height);
					out["segments"] = uint_value(p.segments);
				}
				void operator()(const capsule_params& p) const
				{
					out["radius"] = float_value(p.radius);
					out["height"] = float_value(p.height);
					out["segments"] = uint_value(p.segments);
					out["hemisphere_rings"] = uint_value(p.hemisphere_rings);
				}
			};
			std::visit(writer{ out }, params);
			return out;
		}

		shape_params params_from_json(std::string_view kind, const json::object& object, const std::string& where)
		{
			std::optional<shape_params> params = default_shape_params(kind);
			if (!params) {
				fail(where, std::format("unknown shape kind '{}'", kind.substr(0, 32)));
			}
			struct reader
			{
				const json::object& object;
				const std::string& where;
				void operator()(box_params& p) const
				{
					if (const json::value* size = find(object, "size")) {
						const json::array& items = tuple_of(*size, 3, where + ".size");
						p.size = { float_of(items[0], where + ".size[0]"), float_of(items[1], where + ".size[1]"), float_of(items[2], where + ".size[2]") };
					}
				}
				void operator()(plane_params& p) const
				{
					if (const json::value* size = find(object, "size")) {
						const json::array& items = tuple_of(*size, 2, where + ".size");
						p.size = { float_of(items[0], where + ".size[0]"), float_of(items[1], where + ".size[1]") };
					}
					p.segments_x = optional_uint(object, "segments_x", p.segments_x, where);
					p.segments_z = optional_uint(object, "segments_z", p.segments_z, where);
				}
				void operator()(sphere_params& p) const
				{
					p.radius = optional_float(object, "radius", p.radius, where);
					p.segments = optional_uint(object, "segments", p.segments, where);
					p.rings = optional_uint(object, "rings", p.rings, where);
				}
				void operator()(cylinder_params& p) const
				{
					p.radius = optional_float(object, "radius", p.radius, where);
					p.height = optional_float(object, "height", p.height, where);
					p.segments = optional_uint(object, "segments", p.segments, where);
				}
				void operator()(capsule_params& p) const
				{
					p.radius = optional_float(object, "radius", p.radius, where);
					p.height = optional_float(object, "height", p.height, where);
					p.segments = optional_uint(object, "segments", p.segments, where);
					p.hemisphere_rings = optional_uint(object, "hemisphere_rings", p.hemisphere_rings, where);
				}
			};
			std::visit(reader{ object, where }, *params);
			return *params;
		}
	}

	void poly_mesh_from_json(const json::object& resource, poly_mesh_desc& out, const poly_read_limits& limits)
	{
		poly_mesh mesh;
		std::optional<shape_params> generator;

		if (const json::value* generator_value = find(resource, "generator")) {
			if (!generator_value->is_object()) {
				fail("generator", "is not an object");
			}
			const json::object& object = generator_value->get_object();
			const json::value* kind = find(object, "kind");
			if (!kind || !kind->is_string()) {
				fail("generator.kind", "is missing or not a string");
			}
			const json::value* params = find(object, "params");
			if (params && !params->is_object()) {
				fail("generator.params", "is not an object");
			}
			static const json::object no_params;
			generator = params_from_json(std::string_view(kind->get_string()), params ? params->get_object() : no_params, "generator.params");
		}

		if (const json::value* vertices_value = find(resource, "vertices")) {
			const json::array& vertices = array_of(*vertices_value, "vertices");
			if (vertices.size() > limits.vertices) {
				fail("vertices", std::format("holds {} vertices, at most {} are read", vertices.size(), limits.vertices));
			}
			for (std::size_t i = 0; i < vertices.size(); ++i) {
				const std::string where = std::format("vertices[{}]", i);
				const json::array& item = tuple_of(vertices[i], 4, where);
				const vertex_id id = uint_of(item[0], where + "[0]");
				const glm::vec3 position{ float_of(item[1], where + "[1]"), float_of(item[2], where + "[2]"), float_of(item[3], where + "[3]") };
				if (!mesh.restore_vertex(id, position)) {
					fail(where, std::format("id {} is 0, 4294967295 or used twice", id));
				}
			}
		}

		if (const json::value* faces_value = find(resource, "faces")) {
			const json::array& faces = array_of(*faces_value, "faces");
			if (faces.size() > limits.faces) {
				fail("faces", std::format("holds {} faces, at most {} are read", faces.size(), limits.faces));
			}
			std::size_t corner_total = 0;
			for (std::size_t i = 0; i < faces.size(); ++i) {
				const std::string where = std::format("faces[{}]", i);
				if (!faces[i].is_object()) {
					fail(where, "is not an object");
				}
				const json::object& object = faces[i].get_object();
				const json::value* id_value = find(object, "id");
				const json::value* corners_value = find(object, "corners");
				if (!id_value || !corners_value) {
					fail(where, "needs `id` and `corners`");
				}
				const face_id id = uint_of(*id_value, where + ".id");
				const std::uint32_t slot = optional_uint(object, "material", 0, where);
				if (slot > limits.material_slot) {
					fail(where + ".material", std::format("is {}, at most {} is read", slot, limits.material_slot));
				}
				const json::array& corners_json = array_of(*corners_value, where + ".corners");
				if (corners_json.size() > limits.face_corners) {
					fail(where + ".corners", std::format("holds {} corners, at most {} are read", corners_json.size(), limits.face_corners));
				}
				corner_total += corners_json.size();
				if (corner_total > limits.corners) {
					fail(where, std::format("is past the limit of {} corners in a mesh", limits.corners));
				}
				std::vector<corner> corners;
				corners.reserve(corners_json.size());
				for (std::size_t c = 0; c < corners_json.size(); ++c) {
					const std::string corner_where = std::format("{}.corners[{}]", where, c);
					const json::array& item = tuple_of(corners_json[c], 3, corner_where);
					corners.push_back({ uint_of(item[0], corner_where + "[0]"),
						glm::vec2{ float_of(item[1], corner_where + "[1]"), float_of(item[2], corner_where + "[2]") } });
				}
				if (!mesh.restore_face(id, std::move(corners), slot)) {
					fail(where, "has an id that is 0, 4294967295 or used twice, fewer than 3 corners, an unknown vertex or a vertex twice in a row");
				}
			}
		}

		if (const json::value* edges_value = find(resource, "edges")) {
			const json::array& edges = array_of(*edges_value, "edges");
			if (edges.size() > limits.edges) {
				fail("edges", std::format("holds {} entries, at most {} are read", edges.size(), limits.edges));
			}
			for (std::size_t i = 0; i < edges.size(); ++i) {
				const std::string where = std::format("edges[{}]", i);
				const json::array& item = tuple_of(edges[i], 3, where);
				const edge_key key = edge_key::of(uint_of(item[0], where + "[0]"), uint_of(item[1], where + "[1]"));
				const std::uint32_t flags = uint_of(item[2], where + "[2]");
				if (flags > 3 || !mesh.find_vertex(key.a) || !mesh.find_vertex(key.b) || key.a == key.b) {
					fail(where, "names a missing vertex, one vertex twice or flags other than 1, 2 and 3");
				}
				mesh.set_edge_flags(key, static_cast<edge_flag>(flags));
			}
		}

		out.mesh = std::move(mesh);
		out.generator = std::move(generator);
	}

	json::object shape_params_to_json(const shape_params& params)
	{
		return params_to_json(params);
	}

	std::optional<shape_params> shape_params_from_json(std::string_view kind, const json::object& params, std::string& error)
	{
		try {
			return params_from_json(kind, params, "params");
		} catch (const std::exception& failure) {
			error = failure.what();
			return std::nullopt;
		}
	}

	json::object poly_mesh_to_json(const poly_mesh_desc& source)
	{
		json::object out;
		out["__type"] = json::string(poly_type_name);
		if (source.generator) {
			json::object generator;
			generator["kind"] = json::string(shape_kind_name(*source.generator));
			generator["params"] = params_to_json(*source.generator);
			out["generator"] = std::move(generator);
		}

		json::array vertices;
		vertices.reserve(source.mesh.vertices().size());
		for (const auto& [id, vertex_data] : source.mesh.vertices()) {
			vertices.push_back(json::array{ uint_value(id), float_value(vertex_data.position.x), float_value(vertex_data.position.y),
				float_value(vertex_data.position.z) });
		}
		out["vertices"] = std::move(vertices);

		json::array faces;
		faces.reserve(source.mesh.faces().size());
		for (const auto& [id, face_data] : source.mesh.faces()) {
			json::array corners;
			corners.reserve(face_data.corners.size());
			for (const corner& c : face_data.corners) {
				corners.push_back(json::array{ uint_value(c.vertex), float_value(c.uv.x), float_value(c.uv.y) });
			}
			json::object face_json;
			face_json["id"] = uint_value(id);
			face_json["material"] = uint_value(face_data.material_slot);
			face_json["corners"] = std::move(corners);
			faces.push_back(std::move(face_json));
		}
		out["faces"] = std::move(faces);

		json::array edges;
		for (const auto& [key, flags] : source.mesh.flagged_edges()) {
			edges.push_back(json::array{ uint_value(key.a), uint_value(key.b), uint_value(static_cast<std::uint8_t>(flags)) });
		}
		out["edges"] = std::move(edges);
		return out;
	}

	void poly_mesh_desc::deserialize(desc::desc_system&, const json::object& resource)
	{
		poly_mesh_from_json(resource, *this);
	}

	void poly_mesh_desc::serialize(json::object& resource) const
	{
		resource = poly_mesh_to_json(*this);
	}

	std::string write_geometry_json(const rnd::geometry_desc& geometry, const geometry_file_options& options)
	{
		json::object object;
		geometry.serialize(object);
		object.erase("bounds");
		if (options.write_parent) {
			object["__parent"] = "res://base_geometry.desc";
		}
		return json::serialize(object);
	}

	void register_mesh_descs(desc::desc_system& system)
	{
		system.register_desc<poly_mesh_desc>(poly_type_name);
	}

	void unregister_mesh_descs(desc::desc_system& system)
	{
		system.unregister_desc(poly_type_name);
	}
}
