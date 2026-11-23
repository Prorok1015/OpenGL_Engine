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
		// The offsets, sizes and layouts of the vertex and index chunks of two geometry descs (the hashes are left out).
		bool same_chunks(const json::value& made, std::string_view disk_text)
		{
			boost::system::error_code parse_error;
			const json::value disk = json::parse(disk_text, parse_error);
			if (parse_error || !disk.is_object() || !made.is_object()) return false;
			for (const char* chunk : { "vertices", "indices" }) {
				const json::value* wanted = made.get_object().if_contains(chunk);
				const json::value* found = disk.get_object().if_contains(chunk);
				if (!wanted || !found || !wanted->is_object() || !found->is_object()) return false;
				json::object left = wanted->get_object();
				json::object right = found->get_object();
				left.erase("sha256");
				right.erase("sha256");
				if (left != right) return false;
			}
			return true;
		}

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

	std::string poly_file_text(const poly_mesh_desc& source)
	{
		return repository_text(poly_mesh_to_json(source), poly_file_version);
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

	std::optional<material_range> poly_text_range(std::string_view poly_text, std::string& error, std::string_view disk_desc)
	{
		boost::system::error_code parse_error;
		const json::value parsed = json::parse(poly_text, parse_error);
		if (parse_error || !parsed.is_object()) {
			error = "the file is not a JSON object";
			return std::nullopt;
		}
		poly_mesh_desc source;
		try {
			poly_mesh_from_json(parsed.get_object(), source);
		} catch (const std::exception& failure) {
			error = failure.what();
			return std::nullopt;
		}
		const baked_mesh baked = bake(source.mesh);
		if (baked.ranges.size() != 1) {
			error = baked.ranges.empty() ? "the mesh has no triangles" : "the mesh has several materials: one node draws one of them";
			return std::nullopt;
		}
		if (!disk_desc.empty()) {
			std::string encode_error;
			const geometry_pair made = encode_geometry_pair(res::tag{ std::string("res://") + "check.geom.desc" }, baked.geometry, encode_error);
			if (!encode_error.empty()) {
				error = encode_error;
				return std::nullopt;
			}
			if (!same_chunks(json::parse(made.desc), disk_desc)) {
				error = "the geometry on disk is not the bake of this source (it was edited or rolled back without shape.rebuild): run shape.rebuild";
				return std::nullopt;
			}
		}
		return baked.ranges.front();
	}

	geometry_match geometry_is_bake_of(std::string_view poly_text, std::string_view geometry_desc, std::string& error)
	{
		boost::system::error_code parse_error;
		const json::value parsed = json::parse(poly_text, parse_error);
		if (parse_error || !parsed.is_object()) {
			error = "the source is not a JSON object";
			return geometry_match::unchecked;
		}
		poly_mesh_desc source;
		try {
			poly_mesh_from_json(parsed.get_object(), source);
		} catch (const std::exception& failure) {
			error = failure.what();
			return geometry_match::unchecked;
		}
		const baked_mesh baked = bake(source.mesh);
		const geometry_pair made = encode_geometry_pair(res::tag{ std::string("res://") + "check.geom.desc" }, baked.geometry, error);
		if (!error.empty()) return geometry_match::unchecked;
		boost::system::error_code disk_error;
		const json::value disk = json::parse(geometry_desc, disk_error);
		if (disk_error || !disk.is_object() || !same_chunks(json::parse(made.desc), geometry_desc)) {
			return geometry_match::differs;
		}
		return geometry_match::same;
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
