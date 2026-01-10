#pragma once
#include "desc_base.hpp"

namespace scn
{
	class level_desc : public desc::desc_base
	{
	public:
		level_desc() = default;
		~level_desc() = default;
		level_desc(const level_desc&) = default;
		level_desc(level_desc&&) = default;
		level_desc& operator=(const level_desc&) = default;
		level_desc& operator=(level_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			level_desc& other_desc = static_cast<level_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
	};
}