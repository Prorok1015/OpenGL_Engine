#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "rnd_buffer_layout.h"
#include "ds_rtree.hpp"

namespace rnd
{
	class geometry_desc : public desc::desc_base
	{
	public:
		geometry_desc() = default;
		virtual ~geometry_desc() override = default;

		virtual void copy_to(desc::desc_base& other) const override
		{
			geometry_desc& other_desc = static_cast<geometry_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(json::object& resource) const override;

	public:
		rnd::driver::BufferLayout layout;
		std::vector<unsigned int> indices;
		std::vector<std::byte> vertices;
		std::vector<std::pair<ds::bbox, uint32_t>> bounds;
		ds::rtree<uint32_t, ds::bbox, ds::point2d> rtree;
	};
}