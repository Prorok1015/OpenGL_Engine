#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "rnd_buffer_layout.h"

namespace rnd
{
	class geometry_desc : public desc::desc_base
	{
	public:
		geometry_desc() = default;
		virtual ~geometry_desc() override = default;
		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object& resource) const override;

	public:
		rnd::driver::BufferLayout layout;
		std::vector<unsigned int> indices;
		std::vector<std::byte> vertices;
	};
}