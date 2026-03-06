#pragma once
#include "res_tag.h"
#include <memory>

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
	protected:
		void set_tag(const tag& tag) {
			m_tag = tag;
		}
	protected:
		tag m_tag;
	};

	using RecourceRef = std::shared_ptr<resource_entry>;
}
