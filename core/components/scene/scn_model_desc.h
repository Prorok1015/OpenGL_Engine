#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "geom/rnd_geometry_desc.h"

namespace scn
{
	class model_desc : public desc::desc_base
	{
	public:
		// Inherited via desc_base
		void deserialize(desc::desc_system& desc_system, const json::object&) override;

		void serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const override;

	private:

	};
}