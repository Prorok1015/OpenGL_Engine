#include <snk/project/prj_recent.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <system_error>
#include <utility>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::int64_t k_format_version = 1;

	// The project may be gone or moved since it was remembered; then its spelling is compared as written.
	fs::path identity_of(const fs::path& manifest_file)
	{
		std::error_code ec;
		const fs::path resolved = fs::weakly_canonical(manifest_file, ec);
		return ec ? manifest_file.lexically_normal() : resolved;
	}

	void add_unique(prj::recent_projects& list, std::vector<fs::path>& seen, const fs::path& manifest_file)
	{
		if (list.size() >= prj::k_max_recent_projects) {
			return;
		}
		fs::path identity = identity_of(manifest_file);
		if (std::find(seen.begin(), seen.end(), identity) != seen.end()) {
			return;
		}
		seen.push_back(std::move(identity));
		list.push_back(manifest_file);
	}
}

prj::recent_projects prj::remember_project(recent_projects list, const fs::path& manifest_file)
{
	recent_projects result;
	std::vector<fs::path> seen;
	add_unique(result, seen, manifest_file);
	for (const auto& entry : list) {
		add_unique(result, seen, entry);
	}
	return result;
}

prj::recent_projects prj::parse_recent_projects(std::string_view text)
{
	std::error_code ec;
	const json::value root = json::parse(text, ec);
	if (ec || !root.is_object()) {
		return {};
	}
	const json::object& object = root.as_object();
	const auto* version = object.if_contains("format_version");
	const auto* projects = object.if_contains("projects");
	if (!version || !version->is_int64() || version->as_int64() != k_format_version || !projects || !projects->is_array()) {
		return {};
	}

	recent_projects list;
	std::vector<fs::path> seen;
	for (const auto& entry : projects->as_array()) {
		if (!entry.is_string()) {
			continue;
		}
		const std::string_view utf8 = entry.as_string();
		const fs::path path{ std::u8string{ utf8.begin(), utf8.end() } };
		if (path.is_absolute()) {
			add_unique(list, seen, path);
		}
	}
	return list;
}

std::string prj::serialize_recent_projects(const recent_projects& list)
{
	json::array projects;
	for (const auto& path : list) {
		const std::u8string utf8 = path.generic_u8string();
		projects.emplace_back(std::string_view{ reinterpret_cast<const char*>(utf8.data()), utf8.size() });
	}
	return json::serialize(json::object{ { "format_version", k_format_version }, { "projects", std::move(projects) } });
}
