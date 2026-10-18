#pragma once
#include <snk/desc/desc_schema.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Attribute builders for described C++ types. Fields come from BOOST_DESCRIBE_CLASS; what Describe cannot
// say (JSON key, range, aliases, ...) goes into a constexpr table specialized per type, matched by member name
// (private members have no usable pointer outside the class):
//
//   template<> inline constexpr auto desc::field_attrs<scn::camera_desc> = desc::attrs(
//       desc::f("m_fov").key("/fov").range(1, 179),
//       desc::f("m_color_targets").alias("/texture").write_if_valid());
namespace desc
{
	inline constexpr std::size_t max_field_aliases = 4;

	struct field_attr_data
	{
		std::string_view member;
		std::string_view key; // empty: "/" + member
		std::string_view tooltip;
		std::string_view ref;
		std::string_view filter;
		std::array<std::string_view, max_field_aliases> aliases{};
		std::size_t alias_count = 0;
		bool has_range = false;
		field_range range;
		bool has_kind = false;
		field_kind kind = field_kind::custom;
		write_policy write = write_policy::always;
		bool optional = false;
		bool hidden = false;
		bool read_only = false;
		bool color = false;
		bool degrees = false;
	};

	class field_attr
	{
	public:
		constexpr field_attr() = default;
		constexpr explicit field_attr(std::string_view member) { m_data.member = member; }

		constexpr field_attr key(std::string_view pointer) const { auto c = *this; c.m_data.key = pointer; return c; }
		constexpr field_attr tooltip(std::string_view text) const { auto c = *this; c.m_data.tooltip = text; return c; }
		constexpr field_attr ref(std::string_view type_name) const { auto c = *this; c.m_data.ref = type_name; return c; }
		constexpr field_attr filter(std::string_view exts) const { auto c = *this; c.m_data.filter = exts; return c; }
		constexpr field_attr kind(field_kind value) const { auto c = *this; c.m_data.has_kind = true; c.m_data.kind = value; return c; }
		constexpr field_attr range(double min, double max, double step = 0.0) const
		{
			auto c = *this;
			c.m_data.has_range = true;
			c.m_data.range = field_range{ min, max, step };
			return c;
		}
		// Extra JSON pointer read when `key` is absent (a renamed or legacy key). Never written.
		constexpr field_attr alias(std::string_view pointer) const
		{
			auto c = *this;
			if (c.m_data.alias_count < max_field_aliases) {
				c.m_data.aliases[c.m_data.alias_count] = pointer;
				++c.m_data.alias_count;
			}
			return c;
		}
		constexpr field_attr write_if_valid() const { auto c = *this; c.m_data.write = write_policy::if_valid; return c; }
		constexpr field_attr write_if_not_default() const { auto c = *this; c.m_data.write = write_policy::if_not_default; return c; }
		constexpr field_attr optional() const { auto c = *this; c.m_data.optional = true; return c; }
		constexpr field_attr hidden() const { auto c = *this; c.m_data.hidden = true; return c; }
		constexpr field_attr read_only() const { auto c = *this; c.m_data.read_only = true; return c; }
		constexpr field_attr color() const { auto c = *this; c.m_data.color = true; return c; }
		constexpr field_attr degrees() const { auto c = *this; c.m_data.degrees = true; return c; }

		constexpr const field_attr_data& data() const { return m_data; }

	private:
		field_attr_data m_data;
	};

	constexpr field_attr f(std::string_view member)
	{
		return field_attr{ member };
	}

	template<std::size_t N>
	struct attr_table
	{
		std::array<field_attr, N> items;

		constexpr const field_attr_data* find(std::string_view member) const
		{
			for (const auto& item : items) {
				if (item.data().member == member) {
					return &item.data();
				}
			}
			return nullptr;
		}
	};

	template<typename... Attrs>
	constexpr auto attrs(Attrs... entries)
	{
		return attr_table<sizeof...(Attrs)>{ std::array<field_attr, sizeof...(Attrs)>{ entries... } };
	}

	template<typename T>
	inline constexpr auto field_attrs = attrs();
}
