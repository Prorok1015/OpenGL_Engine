#pragma once
#include "common.h"
#include "desc_base.hpp"

namespace scn
{
	class model_tree_desc : public desc::desc_base
	{
	public:
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const override;

	private:

	};
}
