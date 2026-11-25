#include "edt_shader_problems.h"
#include "edt_asset_database.h"
#include <snk/common/logger/engine_log.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/rnd_shader_problem.h>
#include <algorithm>
#include <string>
#include <utility>

std::vector<edt::problem> edt::shader_problem_publisher::to_problems(const std::vector<rnd::shader_problem>& items)
{
	std::vector<problem> out;
	out.reserve(items.size());
	for (const rnd::shader_problem& item : items) {
		// The same file built with other defines fails the same way: one row, every program in its notes.
		const auto same = std::ranges::find_if(out, [&item](const problem& row) {
			return row.file == item.file.string() && row.line == item.line && row.column == item.column && row.code == item.code && row.message == item.message;
		});
		if (same != out.end()) {
			if (!item.program.empty() && std::ranges::find(same->notes, item.program) == same->notes.end()) {
				same->notes.push_back(item.program);
			}
			continue;
		}
		problem row;
		row.severity = problem_severity::error;
		row.file = item.file.string();
		row.line = item.line;
		row.column = item.column;
		row.code = item.code;
		row.message = item.message;
		if (!item.program.empty()) {
			row.notes.push_back(item.program);
		}
		out.push_back(std::move(row));
	}
	return out;
}

void edt::shader_problem_publisher::follow(problem_list& problems, std::uint64_t generation, const std::function<std::vector<rnd::shader_problem>()>& snapshot)
{
	if (generation == m_generation) {
		return;
	}
	m_generation = generation;
	if (m_check_fresh) {
		m_check_fresh = false;
	} else {
		m_check.clear();
	}
	std::vector<rnd::shader_problem> items = snapshot();
	for (const rnd::shader_problem& row : m_check) {
		const auto same = std::ranges::find_if(items, [&row](const rnd::shader_problem& known) {
			return known.file == row.file && known.line == row.line && known.message == row.message;
		});
		if (same == items.end()) items.push_back(row);
	}
	std::vector<problem> now = to_problems(items);
	if (now.empty()) {
		if (m_published) {
			problems.clear(source);
		}
		m_published = false;
		return;
	}
	problems.set(source, std::move(now));
	m_published = true;
}

void edt::shader_problem_publisher::set_check(std::vector<rnd::shader_problem> rows)
{
	m_check = std::move(rows);
	m_check_fresh = true;
	m_generation = ~std::uint64_t{ 0 };
}

void edt::shader_problem_publisher::follow(problem_list& problems, const rnd::render_system& renderer)
{
	follow(problems, renderer.shader_problems_generation(), [&renderer] { return renderer.shader_problems(); });
}

bool edt::is_shader_source(std::string_view path)
{
	return asset_type_of(path) == "shader";
}

edt::shader_file_open edt::open_shader_problem_file(const problem& row, const std::function<std::optional<std::filesystem::path>(const res::tag&)>& disk_path,
	const std::function<std::string(const std::filesystem::path&)>& open_file)
{
	if (row.file.empty() || !is_shader_source(row.file)) {
		egLOG_INFO("editor", "not opened: '{}' is not a shader source", row.file);
		return shader_file_open::not_a_shader_source;
	}
	const std::optional<std::filesystem::path> on_disk = disk_path ? disk_path(res::tag{ row.file }) : std::nullopt;
	if (!on_disk) {
		egLOG_INFO("editor", "'{}' is not a file on the disk", row.file);
		return shader_file_open::not_on_disk;
	}
	if (!is_shader_source(on_disk->generic_string())) {
		egLOG_INFO("editor", "not opened: '{}' is not a shader source", on_disk->generic_string());
		return shader_file_open::not_a_shader_source;
	}
	if (!open_file) {
		return shader_file_open::failed;
	}
	if (const std::string error = open_file(*on_disk); !error.empty()) {
		egLOG_WARN("editor", "cannot open '{}': {}", on_disk->generic_string(), error);
		return shader_file_open::failed;
	}
	return shader_file_open::opened;
}
