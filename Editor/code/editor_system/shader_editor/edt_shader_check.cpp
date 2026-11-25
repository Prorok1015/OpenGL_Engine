#include "edt_shader_check.h"
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <format>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
	constexpr std::string_view k_desc_suffix = ".shader.desc";
	constexpr std::size_t k_max_files_walked = 20000;
}

std::vector<res::tag> edt::shader_descs_in(const std::vector<fs::path>& layers)
{
	std::vector<std::string> names;
	std::size_t walked = 0;
	for (const fs::path& root : layers) {
		std::error_code error;
		fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
		for (; !error && it != fs::recursive_directory_iterator{}; it.increment(error)) {
			if (++walked > k_max_files_walked) break;
			if (!it->is_regular_file(error)) continue;
			const std::string name = fs_paths::generic_utf8_of(fs::relative(it->path(), root, error));
			if (!error && name.ends_with(k_desc_suffix)) names.push_back(name);
		}
	}
	std::ranges::sort(names);
	names.erase(std::ranges::unique(names).begin(), names.end());
	std::vector<res::tag> out;
	out.reserve(names.size());
	for (const std::string& name : names) out.push_back(res::tag::make(name));
	return out;
}

edt::shader_check_report edt::check_shader_source(const res::tag& source, const std::vector<res::tag>& descs,
	const std::function<rnd::shader_check_result(const res::tag&)>& check)
{
	shader_check_report report;
	const bool is_desc = source.view().ends_with(k_desc_suffix);
	std::vector<res::tag> candidates;
	if (is_desc) {
		candidates.push_back(source);
	} else {
		candidates = descs;
	}
	if (candidates.size() > k_shader_check_max_programs) {
		candidates.resize(k_shader_check_max_programs);
		report.truncated = true;
	}
	for (const res::tag& desc : candidates) {
		rnd::shader_check_result result = check(desc);
		++report.built;
		if (!is_desc && std::ranges::find(result.files, source) == result.files.end()) continue;
		++report.belonging;
		for (rnd::shader_problem& problem : result.problems) {
			problem.program = result.program.empty() ? std::string{ "default variant" } : "default variant: " + result.program;
			report.problems.push_back(std::move(problem));
		}
	}
	const std::string name(source.name());
	if (report.belonging == 0) {
		report.summary = report.truncated
			? std::format("No program of the first {} shader descs is made of {}: nothing to check.", k_shader_check_max_programs, name)
			: std::format("No shader desc has a program made of {}: nothing to check.", name);
	} else if (report.problems.empty()) {
		report.summary = std::format("Checked {} program(s), default variant: no problems.", report.belonging);
	} else {
		report.summary = std::format("Checked {} program(s), default variant: {} problem(s), see Problems.", report.belonging, report.problems.size());
	}
	if (report.truncated && report.belonging > 0) report.summary += std::format(" Only the first {} shader descs were built.", k_shader_check_max_programs);
	return report;
}
