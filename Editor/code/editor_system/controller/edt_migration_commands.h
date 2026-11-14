#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <snk/project_migration/pmg_apply.h>
#include <snk/resource/res_tag.h>
#include <chrono>
#include <cstdint>
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
	// and, after a person approved the plan they were shown, migrates the project in place across a few frames (a modal,
	// the checks, then the level is opened again). apply_migration runs in the job's io work (#761) while the main thread
	// keeps drawing the modal and runs the engine's desc loads it hands over (apply_context::on_main); what the mounts
	// report waits meanwhile (resource_system::hold_changes). The job answers with the report. Main thread only.
	// project.migrate_rollback {id} (#761) runs prj::migration::rollback_migration through the same run (modal, hand-over
	// to the io work, hold, documents closed and the level opened again) and answers as editor --migrate-rollback exits.
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
		// Called on the main thread before the apply starts (the level's watch); the level load watches again.
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
		// Whether `ticket` was approved by agent_confirmations itself for a retry (agent_confirmations::approved_internally),
		// not by a person; null: never.
		std::function<bool(std::string_view ticket)> approved_internally;
		// What moves whenever the code that migrates can change: the game layer, whose desc types hold the steps, is
		// replaced (a module loaded, reloaded or taken back). Part of the fingerprint; null: 0.
		std::function<std::uint64_t()> code_identity;
		// The migration stopped with an exception: `level` was left detached and must not be saved over the half migrated
		// files until `rolled_back()` says the migration was rolled back or another level is loaded; `reason` says how.
		std::function<void(const res::tag& level, std::function<bool()> rolled_back, const std::string& reason)> level_left_detached;
		// Tests only: stands in for prj::migration::apply_migration (one that throws); null: the real one.
		// Runs on the io lane like the real one; the context's on_main reaches the main thread.
		std::function<prj::migration::apply_outcome(const prj::migration::migration_plan&, const prj::migration::apply_context&)> apply;
		// true once a run starts writing (the apply is handed to the io lane), false when it ended: the editor refuses
		// Play, level changes and saves, builds, module reloads and quitting meanwhile. Null: nothing is told.
		std::function<void(bool running)> running_changed;
		// project.migrate_rollback put migration `id` back: a save block of a migration that stopped may go. Null: nothing.
		std::function<void(const std::string& id)> migration_rolled_back;
		// Tests only: stands in for prj::migration::rollback_migration, on the io lane like it; null: the real one.
		std::function<std::optional<std::string>(const std::filesystem::path& root, std::string_view id,
			const prj::migration::rollback_context& context)> rollback;
	};

	inline constexpr std::string_view k_migration_frame_id = "edt/migrate_project";

	[[nodiscard]] std::vector<registration> register_migration_commands(cmd::command_registry& commands, migration_deps deps);
}
