#include "edt_project_links.h"
#include <snk/common/fs_paths.h>
#include <snk/project/prj_link_check.h>
#include <format>
#include <utility>

namespace
{
	// Far below prj::k_link_walk_limit: the scan runs on the main thread at startup, and code/ is a few hundred files, not a cache.
	constexpr std::size_t k_code_link_scan_limit = 20'000;
}

std::vector<edt::problem> edt::code_link_problems(const prj::project_context& project)
{
	std::vector<problem> out;
	const prj::leaving_links links = prj::links_leaving_project(project.code_dir(), project.root, k_code_link_scan_limit,
		{ project.snk_dir(), project.cache_dir() });
	out.reserve(links.found.size() + 1);
	for (const prj::leaving_link& link : links.found) {
		problem item;
		item.severity = problem_severity::warning;
		item.file = fs_paths::generic_utf8_of(link.link.lexically_relative(project.root));
		item.message = link.target.empty()
			? std::string{ "a link in code/ leads to an unknown place (its target cannot be read, or it is a reparse "
				"point of another kind)" }
			: std::format("a link in code/ leads outside the project (or into its .snk / .cache), to '{}'", fs_paths::generic_utf8_of(link.target));
		item.notes.push_back("the build and a person's reads follow it; the agent's code.* tools do not, and nothing writes through it");
		out.push_back(std::move(item));
	}
	if (!links.complete) {
		problem item;
		item.severity = problem_severity::warning;
		item.file = std::string(prj::k_code_dir_name);
		item.message = "code/ was not checked to the end for links that leave the project";
		item.notes.push_back(std::format("more than {} entries, or an entry that could not be read", k_code_link_scan_limit));
		out.push_back(std::move(item));
	}
	return out;
}
