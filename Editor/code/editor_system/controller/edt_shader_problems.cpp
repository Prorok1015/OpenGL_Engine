#include "edt_shader_problems.h"
#include <snk/render/rnd_render_system.h>
#include <snk/render/rnd_shader_problem.h>
#include <string>
#include <utility>

std::vector<edt::problem> edt::shader_problem_publisher::to_problems(const std::vector<rnd::shader_problem>& items)
{
	std::vector<problem> out;
	out.reserve(items.size());
	for (const rnd::shader_problem& item : items) {
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
	std::vector<problem> now = to_problems(snapshot());
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

void edt::shader_problem_publisher::follow(problem_list& problems, const rnd::render_system& renderer)
{
	follow(problems, renderer.shader_problems_generation(), [&renderer] { return renderer.shader_problems(); });
}
