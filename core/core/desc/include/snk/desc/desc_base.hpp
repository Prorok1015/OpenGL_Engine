#pragma once
#include "common.h"
// json::object appears only as a reference parameter in the pure-virtual serialize/deserialize
// below, so a forward declaration suffices — this keeps full <boost/json.hpp> out of the ~25
// headers that include desc_base.hpp (EPIC-49 US-49-4). TUs that actually call json methods
// include <boost/json.hpp> themselves.
#include <boost/json/fwd.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_base.h>

namespace json = boost::json;

namespace desc
{
	class desc_system;

	class desc_base : public res::resource_entry
	{
		friend desc_system;
	public:
		desc_base() = default;
		// Do not copy m_guid — a copy is a new file, so it must get its own __guid (same as m_tag).
		desc_base(const desc_base& other) : type(other.type) {};
		desc_base(desc_base&&) = default;
		desc_base& operator=(desc_base&&) = default;
		virtual ~desc_base() = default;

		virtual void copy_to(desc_base& other) const  = 0;
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) = 0;
		virtual void serialize(json::object&) const = 0;

		desc_base& operator=(const desc_base& other)
		{
			// Do not copy m_tag or m_guid — resource identity is per-instance.
			type = other.type;
			return *this;
		}

		std::string_view get_type() const { return type; }

		// UUID v4 in lowercase; empty for files without __guid (memory://, inline, not yet saved).
		const std::string& get_guid() const { return m_guid; }
		void set_guid(std::string guid) { m_guid = std::move(guid); }

	protected:
		void set_type(const std::string_view type) { this->type = std::string{ type }; }

	private:
		std::string type;
		std::string m_guid;
	};
}
