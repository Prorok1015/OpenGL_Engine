#include <snk/project_migration/pmg_project_steps.h>
#include <snk/project/prj_manifest.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace
{
	using namespace prj::migration;

	bool manifest_format_needed(const scan_result& scan)
	{
		return scan.manifest.format_version < prj::k_manifest_format_version;
	}

	void manifest_format_apply(project_step_context& context)
	{
		const boost::json::value* before = context.manifest.if_contains("format_version");
		const std::string was = before ? boost::json::serialize(*before) : std::string{ "none" };
		context.manifest["format_version"] = prj::k_manifest_format_version;
		context.notes.push_back(std::format("project.snkproj: format_version {} -> {}", was, prj::k_manifest_format_version));
	}
}

std::string_view prj::migration::to_string(project_phase phase)
{
	return phase == project_phase::pre ? "pre" : "post";
}

void prj::migration::project_step_registry::add(project_step step)
{
	if (step.id.empty() || !step.needed || !step.apply) {
		throw std::invalid_argument(std::format("project step '{}' needs an id, needed and apply", step.id));
	}
	if (find(step.id)) throw std::invalid_argument(std::format("project step '{}' is already registered", step.id));
	m_steps.push_back(std::move(step));
}

const prj::migration::project_step* prj::migration::project_step_registry::find(std::string_view id) const
{
	const auto found = std::ranges::find(m_steps, id, &project_step::id);
	return found == m_steps.end() ? nullptr : &*found;
}

void prj::migration::register_engine_project_steps(project_step_registry& registry)
{
	project_step format;
	format.id = "engine.manifest_format";
	format.phase = project_phase::pre;
	format.summary = std::format("project.snkproj to format_version {}", prj::k_manifest_format_version);
	format.needed = &manifest_format_needed;
	format.apply = &manifest_format_apply;
	registry.add(std::move(format));
}
