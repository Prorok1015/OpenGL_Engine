#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "rnd_texture_interface.h"

namespace rnd
{
	class texture_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		texture_desc() = default;
		virtual ~texture_desc() override = default;

		virtual void copy_to(desc::desc_base& other) const override
		{
			texture_desc& other_desc = static_cast<texture_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(json::object& resource) const override;
		
		const driver::texture_header& get_header() const { return header; }
		const std::string_view get_txm_name() const { return txm_name; }

		driver::texture_header header;
		std::string txm_name;
	};
}