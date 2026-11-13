#pragma once
#include <snk/project_migration/pmg_scan.h>
#include <boost/json/object.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Steps of a whole project, beside the steps of each type (EPIC-58 3.3, #754): what no single file can do on its own.
namespace prj::migration
{
	// pre runs before the files are migrated, post after them.
	enum class project_phase { pre, post };

	std::string_view to_string(project_phase phase);

	// What a project step may change in S4: project.snkproj, held in memory and written once after the post steps,
	// with the stamp of the migration. Files are created and deleted from S5 (#756).
	struct project_step_context
	{
		const std::filesystem::path& root;
		const scan_result& scan;
		boost::json::object& manifest;
		// Lines for the report.
		std::vector<std::string>& notes;
	};

	struct project_step
	{
		// "<owner>.<what>", unique; it is what project.snkproj keeps in applied_steps.
		std::string id;
		project_phase phase = project_phase::pre;
		std::string summary;
		// Whether the project needs the step, from its scan alone; it must be false once the step ran.
		bool (*needed)(const scan_result& scan) = nullptr;
		void (*apply)(project_step_context& context) = nullptr;
	};

	// The steps in the order they were added: the engine's first, then each module's (EPIC-58 3.3).
	class project_step_registry
	{
	public:
		// Refuses an id that is taken or a step without needed/apply (std::invalid_argument).
		void add(project_step step);
		const project_step* find(std::string_view id) const;
		const std::vector<project_step>& steps() const { return m_steps; }

	private:
		std::vector<project_step> m_steps;
	};

	// engine.manifest_format (pre): brings format_version of project.snkproj to prj::k_manifest_format_version.
	void register_engine_project_steps(project_step_registry& registry);
}
