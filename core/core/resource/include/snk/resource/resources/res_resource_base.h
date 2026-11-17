#pragma once
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <memory>
#include <string_view>

namespace res
{
	class resource_system;

	// +1 on any change to the virtual functions below: the module layout key mixes it in, tools/ci/vtable_pins.json pins them (#922).
	inline constexpr std::uint32_t resource_entry_abi_revision = 1;

	class resource_entry
	{
		friend res::resource_system;
	public:
		resource_entry() = default;
		resource_entry(const tag& tag) : m_tag(tag) {}
		virtual ~resource_entry() = default;

		bool operator== (const tag& tag) const { return m_tag == tag; }

		const res::tag& get_tag() const { return m_tag; }

		// What a type mismatch names as the actual type (#615); empty: unnamed.
		virtual std::string_view kind_name() const { return {}; }
	protected:
		void set_tag(const tag& tag) {
			m_tag = tag;
		}
	protected:
		tag m_tag;
	};

}
