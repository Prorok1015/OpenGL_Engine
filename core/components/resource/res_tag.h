#pragma once
#include <glm/glm.hpp>
#include <string>
#include <format>
#include <boost/json.hpp>
#include "engine_assert.h"

namespace json = boost::json;

namespace res
{
	class tag
	{
	public:
		static const tag null;
	
		struct hash {
			std::size_t operator() (const tag& val) const {
				return val.get_hash();
			}
		};

		static const std::string_view default_protocol() { return "res"; }
		static constexpr std::string_view memory = "memory";
		static tag make(const std::string_view path) {
			return tag{ default_protocol(), path };
		}

	public:
		explicit tag(const std::string_view tag_path)
		{
			full_ = tag_path;
			protocol_ = { 0, full_.find_first_of("://") };
			const std::size_t name_start_idx = full_.find_last_of('/');
			const std::size_t path_start_idx = full_.find_first_of('/') + 1;
			path_ = { path_start_idx + 1, name_start_idx - path_start_idx };
			name_ = { name_start_idx + 1, full_.length() - name_start_idx - 1 };
		}

		explicit tag(const std::string_view pref, const std::string_view path) 
		{
			ASSERT_MSG(path.find("://") == std::string_view::npos, "Path already have protocol!");
			full_ = std::vformat("{0}://{1}", std::make_format_args(pref, path));

			protocol_ = { 0, pref.length() };
			const std::size_t name_start_idx = full_.find_last_of("/");
			const std::size_t path_start_idx = full_.find_first_of("/") + 1;
			path_ = { path_start_idx + 1, name_start_idx - path_start_idx };
			name_ = { name_start_idx + 1, full_.length() - name_start_idx - 1 };
		}

		constexpr tag() noexcept = default;
		constexpr tag(const tag&) = default;
		constexpr tag(tag&&) = default;
		constexpr tag& operator= (const tag&) = default;
		constexpr tag& operator= (tag&&) = default;

		constexpr bool is_valid() const noexcept { return !full_.empty(); }

		constexpr const std::string_view protocol() const { return std::string_view(full_.data() + protocol_.x, protocol_.y); }
		constexpr const std::string_view path() const { return std::string_view(full_.data() + path_.x, path_.y); }
		constexpr const std::string_view name() const { return std::string_view(full_.data() + name_.x, name_.y); }
		constexpr const std::string_view pure_name() const { auto result = name(); return result.substr(0, result.find_last_of('.'));  }
		constexpr const std::string_view get_full() const { return full_; }
		constexpr const std::string_view extension() const { return name().substr(name().find_last_of('.') + 1); }

		bool operator== (const tag& val) const { return get_hash() == val.get_hash(); }
		std::size_t get_hash() const { return std::hash<std::string>{}(full_); }
		friend res::tag operator+ (const res::tag& a, const res::tag& b);

	private:
		std::string full_;
		glm::ivec2 protocol_{};
		glm::ivec2 path_{};
		glm::ivec2 name_{};
	};


	void tag_invoke(json::value_from_tag, json::value& out, const res::tag& c);
	res::tag tag_invoke(json::value_to_tag<res::tag>, const json::value& obj);
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

}
