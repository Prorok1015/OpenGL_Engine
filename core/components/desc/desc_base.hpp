#pragma once
#include "common.h"
#include <boost/json.hpp>
#include "res_system.h"

namespace json = boost::json;

namespace desc
{
	class desc_system;

	class desc_base
	{
	public:
		virtual ~desc_base() = default;

		virtual void deserialize(desc_system& desc_system, const json::object&) = 0;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const = 0;
	};
}
