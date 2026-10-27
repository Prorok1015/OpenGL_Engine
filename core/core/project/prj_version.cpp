#include <snk/project/prj_version.h>
#include <charconv>
#include <format>
#include <system_error>

#ifndef SNK_ENGINE_VERSION
#error "SNK_ENGINE_VERSION must be defined by core/core/project/CMakeLists.txt"
#endif

namespace
{
	bool parse_component(std::string_view text, int& out)
	{
		if (text.empty() || text.size() > 9) {
			return false;
		}
		for (const char c : text) {
			if (c < '0' || c > '9') {
				return false;
			}
		}
		const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
		return ec == std::errc{} && end == text.data() + text.size();
	}
}

prj::engine_version prj::current_engine_version()
{
	static const engine_version version = [] {
		const auto parsed = parse_engine_version(SNK_ENGINE_VERSION);
		return parsed.value_or(engine_version{});
	}();
	return version;
}

std::optional<prj::engine_version> prj::parse_engine_version(std::string_view text)
{
	const auto first_dot = text.find('.');
	if (first_dot == std::string_view::npos) {
		return std::nullopt;
	}
	const auto second_dot = text.find('.', first_dot + 1);
	if (second_dot == std::string_view::npos) {
		return std::nullopt;
	}

	engine_version version;
	if (!parse_component(text.substr(0, first_dot), version.major)
		|| !parse_component(text.substr(first_dot + 1, second_dot - first_dot - 1), version.minor)
		|| !parse_component(text.substr(second_dot + 1), version.patch)) {
		return std::nullopt;
	}
	return version;
}

std::string prj::to_string(const engine_version& version)
{
	return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

prj::version_match prj::compare_engine_versions(const engine_version& project, const engine_version& editor)
{
	const auto line = [](const engine_version& v) { return engine_version{ v.major, v.minor, 0 }; };
	if (line(project) < line(editor)) {
		return version_match::project_older;
	}
	if (line(project) > line(editor)) {
		return version_match::project_newer;
	}
	return project.patch == editor.patch ? version_match::exact : version_match::patch_differs;
}
