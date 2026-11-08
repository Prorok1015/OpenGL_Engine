#pragma once
#include <snk/resource/res_tag.h>
#include <snk/common/ds/ds_type_id.hpp>
#include <memory>
#include <string_view>

namespace res
{
	class resource_system;
	class resource_entry
	{
		friend res::resource_system;
	public:
		resource_entry() = default;
		resource_entry(const tag& tag) : m_tag(tag) {}
		virtual ~resource_entry() = default;

		bool operator== (const tag& tag) const { return m_tag == tag; }

		const res::tag& get_tag() const { return m_tag; }

		// For res::diagnostics_collector, which knows a field's expected type only as an id (#615). An entry that does
		// not record its kinds is taken for any.
		virtual bool is_kind_of(ds::type_id) const { return true; }
		virtual std::string_view kind_name() const { return {}; }
	protected:
		void set_tag(const tag& tag) {
			m_tag = tag;
		}
	protected:
		tag m_tag;
	};

}
