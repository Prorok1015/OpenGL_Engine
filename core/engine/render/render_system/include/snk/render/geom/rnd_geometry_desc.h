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

		geometry_desc() = default;
		virtual ~geometry_desc() override = default;

		virtual void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}

		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(json::object& resource) const override;

	public:
		rnd::driver::BufferLayout layout;
		std::vector<unsigned int> indices;
		std::vector<std::byte> vertices;
		std::vector<std::pair<ds::bbox, uint32_t>> bounds;
		ds::rtree_q<uint32_t, ds::bbox> rtree;
	};
}