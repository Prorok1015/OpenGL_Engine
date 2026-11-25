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
			std::error_code entry_error;
			if (!it->is_regular_file(entry_error) || entry_error) continue;
			const std::string name = fs_paths::generic_utf8_of(fs::relative(it->path(), root, entry_error));
			if (!entry_error && name.ends_with(k_desc_suffix)) names.push_back(name);
		}
	}
	std::ranges::sort(names);
	names.erase(std::ranges::unique(names).begin(), names.end());
	std::vector<res::tag> out;
	out.reserve(names.size());
	for (const std::string& name : names) out.push_back(res::tag::make(name));
	return out;
}

edt::shader_check_report edt::check_shader_source(const res::tag& source, const std::vector<res::tag>& descs, const shader_check_ports& ports)
{
	shader_check_report report;
	const bool is_desc = source.view().ends_with(k_desc_suffix);
	std::vector<res::tag> candidates;
	if (is_desc) {
		candidates.push_back(source);
		report.examined = 1;
	} else {
		for (const res::tag& desc : descs) {
			++report.examined;
			const std::vector<res::tag> files = ports.files_of ? ports.files_of(desc) : std::vector<res::tag>{};
			if (std::ranges::find(files, source) != files.end()) candidates.push_back(desc);
		}
	}
	if (candidates.size() > k_shader_check_max_programs) {
		candidates.resize(k_shader_check_max_programs);
		report.truncated = true;
	}
	for (const res::tag& desc : candidates) {
		std::string program;
		std::vector<rnd::shader_problem> problems = ports.check(desc, &program);
		++report.built;
		++report.belonging;
		for (rnd::shader_problem& problem : problems) {
			problem.program = program.empty() ? std::string{ "default variant" } : "default variant: " + program;
			report.problems.push_back(std::move(problem));
		}
	}
	const std::string name(source.name());
	if (report.belonging == 0) {
		report.summary = std::format("No shader desc has a program made of {}: nothing to check.", name);
	} else if (report.problems.empty()) {
		report.summary = std::format("Checked {} program(s), default variant: no problems.", report.belonging);
	} else {
		report.summary = std::format("Checked {} program(s), default variant: {} problem(s), see Problems.", report.belonging, report.problems.size());
	}
	if (report.truncated) report.summary += std::format(" Only the first {} programs were built.", k_shader_check_max_programs);
	return report;
}
