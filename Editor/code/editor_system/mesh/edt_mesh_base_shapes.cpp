#include "edt_mesh_base_shapes.h"
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_pretty_json.h>
#include <boost/json.hpp>
#include <array>

namespace edt::mesh
{
	namespace
	{
		constexpr std::uint32_t round_segments = 24;
		constexpr std::uint32_t round_rings = 12;
		constexpr std::uint32_t capsule_rings = 6;

		const std::array<base_shape, 7> k_base_shapes = { {
			{ "box", box_params{ { 1.f, 1.f, 1.f } } },
			{ "plane", plane_params{ { 1.f, 1.f }, 1, 1 } },
			{ "sphere", sphere_params{ 0.5f, round_segments, round_rings } },
			{ "cylinder", cylinder_params{ 0.5f, 1.f, round_segments } },
			{ "capsule_s", capsule_params{ 0.5f, 0.5f, round_segments, capsule_rings } },
			{ "capsule_m", capsule_params{ 0.5f, 1.f, round_segments, capsule_rings } },
			{ "capsule_l", capsule_params{ 0.5f, 2.f, round_segments, capsule_rings } },
		} };

		baked_mesh bake_of(const base_shape& shape)
		{
			shape_result generated = make_shape(shape.params);
			ASSERT_MSG(generated.mesh.has_value(), "a base shape has parameters its generator refuses");
			return bake(*generated.mesh);
		}
	}

	std::span<const base_shape> base_shapes()
	{
		return k_base_shapes;
	}

	const base_shape* find_base_shape(std::string_view name)
	{
		for (const base_shape& shape : k_base_shapes) {
			if (shape.name == name) {
				return &shape;
			}
		}
		return nullptr;
	}

	std::string base_shape_path(const base_shape& shape)
	{
		return "res://shapes/" + std::string(shape.name) + ".geom.desc";
	}

	std::string base_shape_file_text(const base_shape& shape)
	{
		json::object body;
		bake_of(shape).geometry.serialize(body);

		// The form of a repository desc (DescRepoRoundtripTests, desc_resave --check): the version right after the type, the
		// file's own `bounds` kept, no `__parent`, the text of res::pretty_json_bytes.
		json::object file;
		for (auto& member : body) {
			file.insert_or_assign(member.key(), std::move(member.value()));
			if (member.key() == "__type") {
				file.insert_or_assign("__version", base_shape_file_version);
			}
		}
		const std::vector<std::byte> bytes = desc::pretty_json_bytes(json::value(std::move(file)));
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	material_range base_shape_range(const base_shape& shape)
	{
		const baked_mesh baked = bake_of(shape);
		ASSERT_MSG(baked.ranges.size() == 1, "a base shape is one mesh of one material");
		return baked.ranges.front();
	}
}
