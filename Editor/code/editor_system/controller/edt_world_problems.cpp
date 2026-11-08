#include "edt_world_problems.h"
#include "edt_scene_document.h"
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <algorithm>
#include <format>
#include <string>

namespace {
	const char* kind_name(ecs::diagnostic_kind kind)
	{
		switch (kind) {
		case ecs::diagnostic_kind::unknown_system: return "unknown_system";
		case ecs::diagnostic_kind::duplicate_system: return "duplicate_system";
		case ecs::diagnostic_kind::unknown_phase: return "unknown_phase";
		case ecs::diagnostic_kind::unknown_constraint_target: return "unknown_constraint_target";
		case ecs::diagnostic_kind::wrong_phase_constraint: return "wrong_phase_constraint";
		case ecs::diagnostic_kind::cycle: return "cycle";
		case ecs::diagnostic_kind::access_conflict: return "access_conflict";
		}
		return "";
	}

	bool same(const edt::problem& a, const edt::problem& b)
	{
		return a.severity == b.severity && a.file == b.file && a.code == b.code && a.message == b.message && a.notes == b.notes;
	}
}

std::vector<edt::problem> edt::world_problem_publisher::collect(const scene_document& scene, const scn::context_set* level)
{
	std::vector<problem> out;
	if (!level) return out;
	const std::string file(scene.level_tag.relative());
	const std::size_t count = std::min(scene.world_names.size(), scene.world_descs.size());
	for (std::size_t world = 0; world < count; ++world) {
		const scn::world_context* context = level->find(scene.world_names[world]);
		const ecs::system_plan* plan = context ? context->plan() : nullptr;
		if (!plan) continue;
		const std::vector<std::string>& listed = scene.world_descs[world].get_systems();
		for (const ecs::plan_diagnostic& diagnostic : plan->diagnostics) {
			// The debug check of unordered access is advice for the author of a system, not a fault of the level.
			if (diagnostic.kind == ecs::diagnostic_kind::access_conflict) continue;
			problem item;
			item.severity = diagnostic.is_error() ? problem_severity::error : problem_severity::warning;
			item.file = file;
			item.code = kind_name(diagnostic.kind);
			item.message = std::format("world '{}': {}", scene.world_names[world], diagnostic.message);
			for (std::size_t i = 0; i < listed.size(); ++i) {
				if (diagnostic.message.find("'" + listed[i] + "'") != std::string::npos) {
					item.notes.push_back(std::format("/worlds/{}/systems/{}", world, i));
					break;
				}
			}
			out.push_back(std::move(item));
		}
	}
	return out;
}

void edt::world_problem_publisher::refresh(problem_list& problems, const scene_document& scene, const scn::context_set* level)
{
	std::vector<problem> now = collect(scene, level);
	if (m_published && now.size() == m_last.size() && std::equal(now.begin(), now.end(), m_last.begin(), same)) return;
	if (now.empty()) {
		if (m_published) problems.clear(source);
		m_published = false;
	} else {
		problems.set(source, now);
		m_published = true;
	}
	m_last = std::move(now);
}
