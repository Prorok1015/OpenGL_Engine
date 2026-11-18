#include "edt_mesh_shape_asset.h"
#include "edt_mesh_io.h"
#include <snk/desc/desc_pretty_json.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <boost/json.hpp>
#include <cmath>
#include <variant>
#include <vector>

namespace edt::mesh
{
	namespace
	{
		std::string repository_text(json::object body, int version)
		{
			// The version goes right after the type, as every repository desc has it (DescRepoRoundtripTests, desc_resave --check).
			json::object file;
			for (auto& member : body) {
				const bool is_type = member.key() == "__type";
				file.insert_or_assign(member.key(), std::move(member.value()));
				if (is_type) {
					file.insert_or_assign("__version", version);
				}
			}
			const std::vector<std::byte> bytes = desc::pretty_json_bytes(json::value(std::move(file)));
			return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}
	}

	std::size_t estimate_shape_vertices(const shape_params& params)
	{
		struct counter
		{
			// The two poles are one vertex each, not a ring; parameters the generator refuses must not wrap around.
			static std::size_t minus_poles(std::size_t grid) { return grid > 2 ? grid - 2 : 0; }

			std::size_t operator()(const box_params&) const { return 24; }
			std::size_t operator()(const plane_params& p) const { return (std::size_t{ p.segments_x } + 1) * (std::size_t{ p.segments_z } + 1); }
			std::size_t operator()(const sphere_params& p) const { return minus_poles((std::size_t{ p.segments } + 1) * (std::size_t{ p.rings } + 1)); }
			std::size_t operator()(const cylinder_params& p) const { return 2 * (std::size_t{ p.segments } + 1) + 2 * std::size_t{ p.segments }; }
			std::size_t operator()(const capsule_params& p) const { return minus_poles((std::size_t{ p.segments } + 1) * (2 * std::size_t{ p.hemisphere_rings } + 2)); }
		};
		return std::visit(counter{}, params);
	}

	std::optional<material_range> geometry_file_range(std::string_view text, std::string& error)
	{
		constexpr std::size_t vertex_bytes = 14 * sizeof(float);
		boost::system::error_code parse_error;
		const json::value parsed = json::parse(text, parse_error);
		const json::object* object = parse_error ? nullptr : parsed.if_object();
		const json::value* vertices = object ? object->if_contains("vertices") : nullptr;
		const json::value* indices = object ? object->if_contains("indices") : nullptr;
		if (!vertices || !indices) {
			error = "the file is not a geometry desc";
			return std::nullopt;
		}
		// A desc with ranges (EPIC-79) says the size in bytes of each chunk of its binary file, a desc with arrays holds the values.
		const auto count_of = [](const json::value& chunk, std::size_t unit) -> std::optional<std::size_t> {
			if (chunk.is_array()) return chunk.get_array().size() / unit;
			const json::object* ranges = chunk.if_object();
			const json::value* size = ranges ? ranges->if_contains("size") : nullptr;
			// A whole number JSON gives as signed unless it is past the signed range.
			std::uint64_t bytes = 0;
			if (size && size->is_uint64()) bytes = size->get_uint64();
			else if (size && size->is_int64() && size->get_int64() >= 0) bytes = static_cast<std::uint64_t>(size->get_int64());
			else return std::nullopt;
			return static_cast<std::size_t>(bytes / unit);
		};
		const bool vertex_fit = vertices->is_array() ? vertices->get_array().size() % vertex_bytes == 0 : true;
		const std::optional<std::size_t> vertex_count = count_of(*vertices, vertex_bytes);
		const std::optional<std::size_t> index_count = count_of(*indices, indices->is_array() ? 1 : sizeof(std::uint32_t));
		if (!vertex_count || !index_count) {
			error = "the file is not a geometry desc";
			return std::nullopt;
		}
		if (!vertex_fit || *vertex_count == 0 || *index_count == 0) {
			error = "the vertices are not in the layout of the shapes (position, normal, uv, tangent, bitangent)";
			return std::nullopt;
		}
		material_range range;
		range.vx_end = static_cast<std::uint32_t>(*vertex_count);
		range.ind_end = static_cast<std::uint32_t>(*index_count);
		return range;
	}

	std::string shape_scale_problem(const glm::vec3& scale)
	{
		for (int axis = 0; axis < 3; ++axis) {
			if (!std::isfinite(scale[axis]) || scale[axis] <= 0.f) {
				return "scale components must be finite and greater than 0 (a negative scale turns the mesh inside out)";
			}
		}
		return {};
	}

	geometry_pair encode_geometry_pair(const res::tag& desc_file, const rnd::geometry_desc& geometry, std::string& error)
	{
		static_assert(geometry_file_version == rnd::geometry_binary_version, "a shape's geometry is a desc with ranges");
		rnd::geometry_file_options options;
		options.bounds = !geometry.bounds.empty();
		rnd::geometry_files files = rnd::encode_geometry_files(desc_file, geometry, options);
		if (!files) {
			error = std::move(files.error);
			return {};
		}
		// The form of a repository desc (DescRepoRoundtripTests, desc_resave --check): the text of desc::pretty_json_bytes.
		const std::string_view compact(reinterpret_cast<const char*>(files.desc.data()), files.desc.size());
		const std::vector<std::byte> bytes = desc::pretty_json_bytes(json::parse(compact));
		return { std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::move(files.binary) };
	}

	shape_asset_result build_shape_asset(const shape_params& params, const res::tag& geometry_file)
	{
		shape_asset_result out;
		shape_result generated = make_shape(params);
		if (!generated.mesh) {
			out.error = std::move(generated.error);
			return out;
		}
		baked_mesh baked = bake(*generated.mesh);

		poly_mesh_desc source;
		source.mesh = std::move(*generated.mesh);
		source.generator = params;

		shape_asset_files files;
		files.poly = repository_text(poly_mesh_to_json(source), poly_file_version);
		geometry_pair pair = encode_geometry_pair(geometry_file, baked.geometry, out.error);
		if (!out.error.empty()) return out;
		files.geometry = std::move(pair.desc);
		files.binary = std::move(pair.binary);
		files.data = std::move(baked.geometry);
		for (const material_range& range : baked.ranges) {
			files.vertices += range.vx_end - range.vx_begin;
			files.indices += range.ind_end - range.ind_begin;
		}
		out.files = std::move(files);
		return out;
	}

	shape_source_check check_shape_source(std::string_view poly_text)
	{
		shape_source_check out;
		boost::system::error_code parse_error;
		const json::value parsed = json::parse(poly_text, parse_error);
		if (parse_error || !parsed.is_object()) {
			out.error = "the file is not a JSON object";
			return out;
		}
		poly_mesh_desc source;
		try {
			poly_mesh_from_json(parsed.get_object(), source);
		} catch (const std::exception& failure) {
			out.error = failure.what();
			return out;
		}
		if (!source.generator) {
			out.state = shape_source::edited;
			out.error = "the mesh has no generator: it was made by hand";
			return out;
		}
		out.generator = source.generator;
		const shape_asset_result again = build_shape_asset(*source.generator, res::tag{ std::string("res://") + "check.geom.desc" });
		if (!again.files) {
			out.state = shape_source::unreadable;
			out.error = again.error;
			return out;
		}
		if (repository_text(poly_mesh_to_json(source), poly_file_version) != again.files->poly) {
			out.state = shape_source::edited;
			out.error = "the mesh differs from what its generator makes: it was edited by hand";
			return out;
		}
		out.state = shape_source::generated;
		return out;
	}
}
