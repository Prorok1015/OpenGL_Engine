#pragma once
#include <snk/common/common.h>
#include <snk/desc/desc_base.hpp>
#include <snk/driver/rnd_buffer_layout.h>
#include <snk/common/ds/ds_rtree.h>
#include <snk/common/ds/ds_bbox.hpp>
#include <snk/common/snk_api.h>

namespace rnd
{
	class SNK_API geometry_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		// Out of line: a geometry read from a binary file (EPIC-79) keeps that state beside it, not in a member. No move
		// operations, as before: a move copies. A new geometry drops a state left at its address.
		geometry_desc();
		geometry_desc(const geometry_desc& other);
		geometry_desc& operator=(const geometry_desc& other);
		virtual ~geometry_desc() override;

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
	};
}