#include "edt_recent_projects.h"
#include "edt_settings.h"
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <system_error>

namespace {
	constexpr std::string_view k_recent_projects_file = "recent_projects.json";
}

prj::recent_projects edt::read_recent_projects(res::resource_system& res)
{
	return prj::parse_recent_projects(read_setting(res, k_recent_projects_file).value_or(std::string{}));
}

prj::recent_projects edt::remember_recent_project(res::resource_system& res, const std::filesystem::path& manifest_file)
{
	auto list = prj::remember_project(read_recent_projects(res), manifest_file);
	if (!write_setting(res, k_recent_projects_file, prj::serialize_recent_projects(list))) {
		egLOG_WARN("Editor/Project", "the list of recent projects was not saved");
	}
	return list;
}

prj::recent_projects edt::recent_projects_besides(res::resource_system& res,
	const std::optional<std::filesystem::path>& open_manifest)
{
	if (!open_manifest) return read_recent_projects(res);
	auto list = remember_recent_project(res, *open_manifest);
	std::erase_if(list, [&](const std::filesystem::path& entry) {
		std::error_code ec;
		return entry == *open_manifest || std::filesystem::equivalent(entry, *open_manifest, ec);
	});
	return list;
}
