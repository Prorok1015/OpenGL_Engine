#pragma once
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
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
		desc_base(const desc_base& other) : res::resource_entry(), type(other.type) {};
		desc_base(desc_base&&) = default;
		desc_base& operator=(desc_base&&) = default;
		virtual ~desc_base() = default;

		// Copies this desc into `other`. An override assigns member-wise when `other` has its own type
		// (copy_to_same); the default, and any other type, goes through serialize/deserialize.
		virtual void copy_to(desc_base& other) const { copy_through_json(other); }
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

		template<typename T>
		void copy_to_same(const T& self, desc_base& other) const
		{
			if (T* typed = dynamic_cast<T*>(&other)) {
				*typed = self;
				return;
			}
			copy_through_json(other);
		}

		// serialize(this) -> deserialize(other): only keys both types know cross over. Needs the desc_system the
		// instances were created by; without one nothing is copied.
		SNK_API void copy_through_json(desc_base& other) const;

	private:
		std::string type;
		std::string m_guid;
		desc_system* m_system = nullptr;
	};
}
