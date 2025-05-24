#pragma once
#include "common.h"
#include "scn_animatable_prototype_desc.h"

namespace scn
{
	class skinning_prototype_desc : public animatable_prototype_desc
	{
	public:
		virtual void copy_to(desc::desc_base& other) const override
		{
			skinning_prototype_desc& other_desc = static_cast<skinning_prototype_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
	};
}