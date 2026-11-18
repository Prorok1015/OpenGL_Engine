#pragma once
#include <snk/common/common.h>
#include <snk/desc/desc_base.hpp>
#include <snk/driver/rnd_buffer_layout.h>
#include <snk/common/ds/ds_rtree.h>
#include <snk/common/ds/ds_bbox.hpp>
#include <snk/common/snk_api.h>
#include <snk/model_codec/res_desc_binary_codec.h>
#include <cstdint>
#include <optional>

namespace rnd::detail
{
	// What deserialize recorded of the binary file beside the desc (EPIC-79, #1068); rnd_geometry_binary.h reads and
	// writes it.
	enum class geometry_binary_state : std::uint8_t
	{
		// Arrays only, or nothing read yet.
		none,
		// Ranges recorded, the data not read: a desc read through the generic desc adapter.
		not_loaded,
		// The geometry adapter filled the vectors from the binary file.
		loaded,
	};

	struct geometry_binary_ranges
	{
		std::optional<res::binary_range> vertices;
		std::optional<res::binary_range> indices;
		// The file has "bounds": they are built again from the checked data, never read.
		bool wants_bounds = false;
	};

	struct geometry_binary
	{
		geometry_binary_state state = geometry_binary_state::none;
		geometry_binary_ranges ranges;
	};
}

namespace rnd
{
	class SNK_API geometry_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		geometry_desc() = default;
		virtual ~geometry_desc() override = default;

		virtual void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}

		// "vertices" and "indices" are arrays or ranges of the binary file next to the desc; only the geometry adapter
		// reads ranges, here they are recorded.
		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		// Throws std::runtime_error for a geometry read from a binary file (never written back as arrays) and for one with a
		// layout but no vertices (never written as an empty mesh).
		virtual void serialize(json::object& resource) const override;

	public:
		rnd::driver::BufferLayout layout;
		std::vector<unsigned int> indices;
		std::vector<std::byte> vertices;
		std::vector<std::pair<ds::bbox, uint32_t>> bounds;
		ds::rtree_q<uint32_t, ds::bbox> rtree;
		// Whether the data came from the binary file; copied with the rest. Only rnd_geometry_binary.h touches it.
		detail::geometry_binary binary;
	};
}