#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <snk/project_migration/pmg_apply.h>
#include <snk/resource/res_tag.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desc
{
	class desc_system;
}

namespace res
{
	class resource_system;
}

namespace edt
{
	class job_service;

	// project.migration_plan / project.migrate (EPIC-58 S7, #759): an agent reads the plan of migrating the open project
	// and, after a person approved the plan they were shown, migrates the project in place. The migration runs on the
	// main thread across a few frames (a modal, then the writes, then the level is opened again); the job of
	// project.migrate only waits for it and answers with the report. Main thread only.
	struct migration_deps
	{
		explicit migration_deps(job_service& job_service_) : jobs(job_service_) {}

		job_service& jobs;
		// The open project's root; nullopt: no project (`no_project`).
		std::function<std::optional<std::filesystem::path>()> project_root;
		std::function<std::string()> project_name;
		// Absolute, below the root: what maps a project path to its res:// tag.
		std::function<std::vector<std::filesystem::path>()> content_roots;
		// The game layer's services, the ones the plan is made with (taken at the moment of use).
		std::function<desc::desc_system&()> descs;
		std::function<res::resource_system&()> resources;
		// Why nothing may migrate now (Play, a load, a build or reload, an import); nullopt: it may.
		std::function<std::optional<std::string>()> busy_reason;
		// Unsaved changes of a document; nullopt: none.
		std::function<std::optional<std::string>()> unsaved_reason;
		// Closes the prefab editor and the data documents, lets go of the level (detach) and returns its tag (invalid: none).
		std::function<res::tag()> close_documents_and_detach_level;
		// Paused while files are written (the level's watch); the level load watches again.
		std::function<void()> pause_watch;
		// After the writes: a file of the project changed on disk.
		std::function<void(const res::tag&)> signal_changed;
		std::function<void()> rebuild_assets;
		// Attaches the level again and, for a valid tag, starts its load from disk; `loaded` is called when the load ends.
		// The error when no load started.
		std::function<std::optional<std::string>(const res::tag& level, std::function<void(bool loaded)> loaded)> reopen_level;
		std::function<bool()> level_load_in_progress;
		std::function<res::tag()> current_level_tag;
		// Called every frame while `fn` returns true.
		std::function<void(std::string_view id, std::function<bool()> fn)> register_implicit;
		// The modal "Migrating project..." (true) or its close (false); null: nothing is drawn (tests).
		std::function<void(bool open)> show_progress;
		// For the lifetime of what a person was shown; null: steady_clock.
		std::function<std::chrono::steady_clock::time_point()> now;
		// Tests only: stands in for prj::migration::apply_migration (one that throws); null: the real one.
		std::function<prj::migration::apply_outcome(const prj::migration::migration_plan&, const prj::migration::apply_context&)> apply;
	};

	inline constexpr std::string_view k_migration_frame_id = "edt/migrate_project";

	[[nodiscard]] std::vector<registration> register_migration_commands(cmd::command_registry& commands, migration_deps deps);
}
