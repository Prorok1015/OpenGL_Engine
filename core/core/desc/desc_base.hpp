#pragma once
#include "common.h"
#include <boost/json.hpp>
#include "res_system.h"
#include "res_tag.h"
#include "resources/res_resource_base.h"

namespace json = boost::json;

namespace desc
{
	class desc_system;

	class desc_base : public res::resource_entry
	{
		friend desc_system;
	public:
		desc_base() = default;
		desc_base(const desc_base&) {};
		desc_base(desc_base&&) = default;
		desc_base& operator=(desc_base&&) = default;
		virtual ~desc_base() = default;

		virtual void copy_to(desc_base& other) const  = 0;
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) = 0;
		virtual void serialize(json::object&) const = 0;

		bool is_loaded() const { return is_loaded_flag; }

		desc_base& operator=(const desc_base& other)
		{
			// HACK: to not copy res::tag m_tag
			return *this;
		}

	private:
		void set_is_loaded(bool f = true) { is_loaded_flag = f; }

	private:
		bool is_loaded_flag = false;
	};
}
