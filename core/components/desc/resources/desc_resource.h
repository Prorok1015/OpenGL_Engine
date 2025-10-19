#pragma once
#include "common.h"
#include "resources/res_resource_base.h"

#include <boost/json.hpp>

namespace json = boost::json;

namespace desc
{
	class desc_resource : public res::resource_entry
	{
	public:
		desc_resource(const res::tag& tag, const json::value& json_data)
			: res::resource_entry(tag)
			, body(json_data.as_object())
		{
		}
		virtual ~desc_resource() = default;
		desc_resource(const desc_resource&) = default;
		desc_resource& operator=(const desc_resource&) = default;
		desc_resource(desc_resource&&) = default;
		desc_resource& operator=(desc_resource&&) = default;

	public:
		json::object body;
	};
}