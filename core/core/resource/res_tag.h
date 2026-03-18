#pragma once
#include <glm/glm.hpp>
#include <string>
#include <format>
#include <ostream>
#include <boost/json.hpp>
#include <algorithm>
#include "engine_assert.h"

namespace json = boost::json;

namespace res
{
	class tag
	{
	public:
		static const tag null;
		static const std::string_view default_protocol() { return "res"; }
		static constexpr std::string_view memory = "memory";
		static tag make(const std::string_view path) {
			return tag{ default_protocol(), path };
		}

	public:
		explicit tag(const std::string_view tag_path)
			: full_(tag_path)
		{
			std::ranges::replace(full_, '\\', '/');
			protocol_ = { 0, full_.find_first_of("://") };
			const std::size_t name_start_idx = full_.find_last_of("/\\");
			const std::size_t path_start_idx = full_.find_first_of("/\\") + 1;
			path_ = { path_start_idx + 1, name_start_idx - path_start_idx };
			name_ = { name_start_idx + 1, full_.length() - name_start_idx - 1 };
		}

		explicit tag(const std::string_view pref, const std::string_view path) 
			: tag(std::vformat("{0}://{1}", std::make_format_args(pref, path)))
		{
			ASSERT_MSG(path.find("://") == std::string_view::npos, "Path already has protocol!");
		}

		constexpr tag() noexcept = default;
		constexpr tag(const tag&) = default;
		constexpr tag(tag&&) = default;
		constexpr tag& operator= (const tag&) = default;
		constexpr tag& operator= (tag&&) = default;
		tag& operator= (const std::string_view str) {
			*this = tag{ str };
			return *this;
		};

		constexpr bool is_valid() const noexcept { return !full_.empty(); }

		constexpr const std::string& string() const { return full_; }
		constexpr const std::string_view protocol() const { return view().substr(protocol_.x, protocol_.y); }
		constexpr const std::string_view path() const { return view().substr(path_.x, path_.y); }
		constexpr const std::string_view name() const { return view().substr(name_.x, name_.y); }
		constexpr const std::string_view pure_name() const { return name().substr(0, name().find_last_of('.'));  }
		constexpr const std::string_view view() const { return full_; }
		constexpr const std::string_view extension() const { return name().substr(name().find_last_of('.') + 1); }
		constexpr const std::string_view relative() const { return view().substr(path_.x); }

		bool operator== (const tag& val) const { return get_hash() == val.get_hash(); }
		std::size_t get_hash() const { return std::hash<std::string>{}(full_); }
		friend res::tag operator+ (const res::tag& a, const res::tag& b);

	private:
		std::string full_;
		glm::ivec2 protocol_{};
		glm::ivec2 path_{};
		glm::ivec2 name_{};
	};

	//TODO: move json dependencies into one place
	void tag_invoke(json::value_from_tag, json::value& out, const res::tag& c);
	res::tag tag_invoke(json::value_to_tag<res::tag>, const json::value& obj);

	inline std::ostream& operator<<(std::ostream& os, const tag& t) {
		return os << t.string();
	}
}

namespace std {
	template <typename T> struct hash;

	template<>
	struct hash<res::tag>
	{
		std::size_t operator()(const res::tag& val) const
		{
			return val.get_hash();
		}
	};

	template<>
	struct formatter<res::tag>
	{
		enum class spec_kind : uint8_t { full, name, path, ext, pure, proto, rel };
		spec_kind kind_ = spec_kind::full;

		constexpr auto parse(std::format_parse_context& ctx) {
			auto it = ctx.begin();
			auto end = ctx.end();
			if (it == end || *it == '}') return it;

			auto spec_begin = it;
			while (it != end && *it != '}') ++it;
			std::string_view spec(&*spec_begin, static_cast<size_t>(std::distance(spec_begin, it)));

			if      (spec == "name")  kind_ = spec_kind::name;
			else if (spec == "path")  kind_ = spec_kind::path;
			else if (spec == "ext")   kind_ = spec_kind::ext;
			else if (spec == "pure")  kind_ = spec_kind::pure;
			else if (spec == "proto") kind_ = spec_kind::proto;
			else if (spec == "rel")   kind_ = spec_kind::rel;

			return it;
		}

		auto format(const res::tag& t, std::format_context& ctx) const {
			std::string_view value;
			switch (kind_) {
			case spec_kind::name:  value = t.name();      break;
			case spec_kind::path:  value = t.path();      break;
			case spec_kind::ext:   value = t.extension(); break;
			case spec_kind::pure:  value = t.pure_name(); break;
			case spec_kind::proto: value = t.protocol();  break;
			case spec_kind::rel:   value = t.relative();  break;
			default:               value = t.view();      break;
			}
			return std::format_to(ctx.out(), "{}", value);
		}
	};
}
