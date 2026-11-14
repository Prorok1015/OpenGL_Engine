#pragma once
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class action_registry;

	// The Project Migration window (EPIC-58 S8, #761) without ImGui: it asks project.migration_plan for the plan, shows it,
	// runs project.migrate as a person's call (origin ui) and shows the report its job answers with (job_finished).
	// Main thread only.
	class migration_window_model
	{
	public:
		struct ports
		{
			// A command of the registry, as a person's call.
			std::function<cmd::result(std::string_view id, boost::json::value args)> invoke;
			// Opens a file of the plan; `path` is relative to the project root, as the plan names it.
			std::function<void(const std::string& path)> open_file;
		};

		// idle: nothing asked yet (or Close); planning: the plan is asked for on the next tick(); plan_shown; running: the
		// migration job runs; report_shown: its answer.
		enum class phase : std::uint8_t { idle, planning, plan_shown, running, report_shown };

		// What project.migration_plan / project.migrate answer with (pmg_command.h).
		inline static constexpr int k_code_done = 0;
		inline static constexpr int k_code_needed = 1;
		inline static constexpr int k_code_refused = 2;
		inline static constexpr int k_code_partial = 4;
		inline static constexpr int k_code_rolled_back = 5;
		inline static constexpr int k_code_rollback_failed = 6;

		struct file_entry
		{
			std::string path;
			// "camera_desc v1 -> v2: <summary>"
			std::vector<std::string> steps;
			std::vector<std::string> warnings;
		};

		struct plan_view
		{
			int code = -1;
			std::vector<file_entry> files;
			// "<id> (<phase>): <summary>"
			std::vector<std::string> project_steps;
			// "<path> (<kind>): <reason>"
			std::vector<std::string> skipped;
			// Why a migration would be refused (an invalid file, an unfinished migration); empty: it would not be.
			std::string refused;
			std::vector<std::string> unfinished;
			// "Engine 0.1.0 -> this engine"; empty when the manifest is current.
			std::string manifest;
			// plan_text: the plan as the command line prints it.
			std::string text;
			// What project.migrate is given as plan_fingerprint: the migration runs only while the project gives this plan.
			std::string fingerprint;
		};

		struct report_view
		{
			// -1: the job answered with an error only (`error_code`).
			int code = -1;
			std::string status;
			std::string id;
			// "<path>: <message>", from the report's files.
			std::vector<std::string> warnings;
			std::string refused;
			// report.md, or the plan's text when nothing was written.
			std::string text;
			// "Level res://levels/a.desc reopened" or why not; empty without a level answer.
			std::string level;
			std::string error_code;
			std::string error_text;
		};

		migration_window_model(editor_events& events, ports ports);
		migration_window_model(const migration_window_model&) = delete;
		migration_window_model& operator=(const migration_window_model&) = delete;

		phase state() const { return m_phase; }
		// Asks for the plan on the next tick() (Refresh, the window opened). Ignored while the migration runs and while a
		// report is shown (it stays until Close).
		void request_plan();
		// Runs what request_plan asked for; the window calls it every frame it is drawn.
		void tick();
		// Only for a plan that needs a migration (code 1).
		bool can_migrate() const { return m_phase == phase::plan_shown && m_plan && m_plan->code == k_code_needed; }
		// project.migrate; refused at once (busy, needs_input, ...) the plan stays shown with the reason.
		void migrate();
		// Back to idle; ignored while the migration runs.
		void close();
		void open_file(const std::string& path) const;

		const std::optional<plan_view>& plan() const { return m_plan; }
		const std::optional<report_view>& report() const { return m_report; }
		std::optional<std::uint64_t> job() const { return m_job; }

		// The last refusal or failure (planning, the call of project.migrate): a code, its text and what to do about it.
		const std::string& error_code() const { return m_error_code; }
		const std::string& error_text() const { return m_error_text; }
		const std::string& error_hint() const { return m_error_hint; }
		// Why the plan shown was made again by itself (the project changed since it was shown); empty otherwise.
		const std::string& notice() const { return m_notice; }

		// What the person can do about an error code of these commands; empty: nothing to add.
		static std::string hint_of(std::string_view code);
		// One line for a report's code: "Migrated", "Rolled back", ...
		static std::string outcome_text(int code);

	private:
		void finished(std::uint64_t id, job_status status, const std::string& error, const std::optional<boost::json::value>& result);
		void fail(std::string code, std::string_view text);
		void clear_error();

		editor_events& m_events;
		ports m_ports;
		phase m_phase = phase::idle;
		std::optional<plan_view> m_plan;
		std::optional<report_view> m_report;
		std::optional<std::uint64_t> m_job;
		std::string m_error_code;
		std::string m_error_text;
		std::string m_error_hint;
		std::string m_notice;
		// Last: gone before the state its slot touches.
		connection m_finished;
	};

	inline constexpr std::string_view k_migration_window_action = "project.migrate.open";

	// Project > Migrate Project...: opens the window for a person. Category not_for_agents and not a tool; enabled only
	// while `has_project` says a project is open.
	[[nodiscard]] registration register_migration_window_action(action_registry& actions, std::function<bool()> has_project,
		std::function<void()> open);
}
