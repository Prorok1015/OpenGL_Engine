#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "rnd_texture_interface.h"

namespace rnd
{
	class texture_desc : public desc::desc_base
	{
	public:
		texture_desc() = default;
		virtual ~texture_desc() override = default;
		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object& resource) const override;
	
		driver::texture_header header;
		std::string txm_name;
		res::tag txm_tag;
	};
}