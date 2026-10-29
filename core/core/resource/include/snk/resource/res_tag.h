#pragma once
#include <cstdint>
#include <string>
#include <format>
#include <ostream>
#include <algorithm>
#include <snk/common/engine_assert.h>
#include <snk/common/snk_api.h>

// Note: JSON (de)serialization of res::tag lives in res_tag_json.h (EPIC-49 US-49-4), so this
// header pulls in neither <boost/json.hpp> nor <glm/glm.hpp>.

namespace res
{
	class SNK_API tag
	{
	public:
		static const tag null;
		static const std::string_view default_protocol() { return "res"; }
		static constexpr std::string_view memory = "memory";
		// The engine's base resources only, read-only: the layer res:// falls back to, without a project's overrides.
		static constexpr std::string_view engine = "engine";
		// <project>/.cache: regenerated data (asset database, thumbnails, autosave), never committed. memory:// and
		// user:// (the editor's test profile) are folders inside it, so cache://memory/... and cache://user_profile/...
		// alias them: clearing the cache must leave those two folders alone.
		static constexpr std::string_view cache = "cache";
		// <project>/code: the game module's sources as text, mounted only when the folder exists (#450). Never an asset
		// protocol: res:// does not see it and it does not see assets.
		static constexpr std::string_view code = "code";
		// The player's data (saves, settings); a test profile while the editor runs.
		static constexpr std::string_view user = "user";
		static tag make(const std::string_view path) {
			return tag{ default_protocol(), path };
		}

	public:
		explicit tag(const std::string_view tag_path)
			: full_(tag_path)
		{
			std::ranges::replace(full_, '\\', '/');
			protocol_ = { 0, static_cast<int>(full_.find_first_of("://")) };
			const std::size_t name_start_idx = full_.find_last_of("/\\");
			const std::size_t path_start_idx = full_.find_first_of("/\\") + 1;
			path_ = { static_cast<int>(path_start_idx + 1), static_cast<int>(name_start_idx - path_start_idx) };
			name_ = { static_cast<int>(name_start_idx + 1), static_cast<int>(full_.length() - name_start_idx - 1) };
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

		bool operator== (const tag& val) const { return full_ == val.full_; }
		std::size_t get_hash() const { return std::hash<std::string>{}(full_); }
		friend SNK_API res::tag operator+ (const res::tag& a, const res::tag& b);

	private:
		// {offset, length} spans into full_ (was glm::ivec2; POD-ified to drop the glm include).
		struct span { int x = 0; int y = 0; };
		std::string full_;
		span protocol_{};
		span path_{};
		span name_{};
	};

	// Namespace-scope declaration: the friend declaration above cannot carry the export attribute.
	SNK_API res::tag operator+ (const res::tag& a, const res::tag& b);

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
