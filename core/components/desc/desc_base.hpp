#pragma once
#include "common.h"
#include <boost/json.hpp>
#include "res_system.h"
#include "res_tag.h"

namespace json = boost::json;

namespace desc
{
	class desc_system;

	class desc_base
	{
		friend desc_system;
	public:
		virtual ~desc_base() = default;

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) = 0;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const = 0;
		bool is_loaded() const { return is_loaded_flag; }
		const res::tag& get_tag() const { return tag; }

	private:
		void set_is_loaded(bool f = true) { is_loaded_flag = f; }
		void set_tag(const res::tag& t) { tag = t; }

	private:
		bool is_loaded_flag = false;
		res::tag tag;
	};
}
