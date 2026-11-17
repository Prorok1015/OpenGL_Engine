#include "edt_mesh_shape_asset.h"
#include "edt_mesh_io.h"
#include <snk/desc/desc_pretty_json.h>
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
		if (!vertices || !vertices->is_array() || !indices || !indices->is_array()) {
			error = "the file is not a geometry desc";
			return std::nullopt;
		}
		if (vertices->get_array().size() % vertex_bytes != 0) {
			error = "the vertices are not in the layout of the shapes (position, normal, uv, tangent, bitangent)";
			return std::nullopt;
		}
		material_range range;
		range.vx_end = static_cast<std::uint32_t>(vertices->get_array().size() / vertex_bytes);
		range.ind_end = static_cast<std::uint32_t>(indices->get_array().size());
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

	std::string geometry_file_text(const rnd::geometry_desc& geometry)
	{
		json::object body;
		geometry.serialize(body);
		return repository_text(std::move(body), geometry_file_version);
	}

	shape_asset_result build_shape_asset(const shape_params& params)
	{
		shape_asset_result out;
		shape_result generated = make_shape(params);
		if (!generated.mesh) {
			out.error = std::move(generated.error);
			return out;
		}
		const baked_mesh baked = bake(*generated.mesh);

		poly_mesh_desc source;
		source.mesh = std::move(*generated.mesh);
		source.generator = params;

		shape_asset_files files;
		files.poly = repository_text(poly_mesh_to_json(source), poly_file_version);
		files.geometry = geometry_file_text(baked.geometry);
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
		const shape_asset_result again = build_shape_asset(*source.generator);
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
