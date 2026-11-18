#pragma once
#include "edt_asset_commands.h"
#include "edt_asset_database.h"
#include "edt_asset_index.h"
#include "edt_asset_operation_form.h"
#include "edt_asset_picker.h"
#include "edt_asset_problems.h"
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace desc { class desc_system; }
namespace prj { struct manifest; struct project_context; }
namespace res { class resource_system; }

namespace edt
{
	class asset_browser_panel;
	class asset_history;
	class asset_ops;
	class editor_context;
	struct problem;

	// Reads the manifest file as it is on the disk and takes the two references asset_ops rewrites, start_scene and input_actions,
	// into `data`; the rest of `data` (modules, content roots, ...) stays as it was loaded, as a running project depends on it.
	// Returns why it could not (a file that is gone, too big or not a manifest); `data` is not changed then.
	std::string refresh_manifest_references(const std::filesystem::path& manifest_file, prj::manifest& data);

	// The asset database of an open project and what hangs on it, whole: the database that follows res:// (#760), the catalog
	// the reference pickers choose from, resource_system's asset index over it (#770), the Problems source "assets" with the
	// person's fixes and the undo of the last one, and asset.usages with the browser's usage list. editor_system creates it only
	// with a project, so every caller of it asks a pointer; it calls install() where the browser's usage list is wired.
	class asset_component
	{
	public:
		struct ports
		{
			editor_context& context;
			// The base system: its layers are what the database follows and the index attaches to.
			res::resource_system& resources;
			asset_browser_panel& browser;
			// The project context lives in the app data storage as long as the editor.
			const prj::project_context& project;
			// A git write runs: files it holds back wait for their ID (#765).
			std::function<bool()> vcs_busy{};
			std::function<std::optional<std::filesystem::path>()> git_dir{};
			// The game's descs and layer generation are looked up when used: a game module replaces both.
			std::function<const desc::desc_system*()> descs{};
			std::function<std::uint64_t()> game_layer_generation{};
			// Why a write to the project must wait (a migration runs, #761); an agent's asset commands answer busy.
			std::function<std::optional<std::string>()> write_blocker{};
			// A file with unsaved edits in an open document, or the open level: an operation on it or on a file that refers to it is refused (#775).
			std::function<bool(const res::tag&)> is_open_with_unsaved_changes{};
			// The manifest inside `project`, which the editor may change (the project context lives in the app data storage): after an
			// operation or its undo rewrote the manifest file, start_scene and input_actions are read from it again (#775). Main thread
			// only, as the operations are; null: the copy stays as it was loaded.
			prj::manifest* manifest_data = nullptr;
		};

		explicit asset_component(ports from);
		~asset_component();
		asset_component(const asset_component&) = delete;
		asset_component& operator=(const asset_component&) = delete;

		// asset.usages, asset.rename/move/delete and the asset browser's usage list.
		void install(std::vector<registration>& out);
		// The `editor_assets` frame stage: files waiting for an ID, the index, the Problems rows.
		void tick();
		void rebuild() { m_database->rebuild(); }

		asset_database& database() { return *m_database; }
		const picker::catalog& catalog() const { return *m_catalog; }
		// For asset_index_wait (#968): why an agent's guid cannot be judged yet.
		const asset_index_link* index() const { return m_index.get(); }

		// The fixes of a Problems row of the source "assets", for the person only.
		std::vector<problem_action> problem_actions(const problem& row);
		std::optional<std::string> problem_file(const problem& row) const { return m_problems->file_of(row); }
		// Rename, move and delete of one asset (#775) and the history of the operations done: they were rebuilt at construction
		// from the editor's own list in the person's data and the journals in cache://asset_ops/ (the newest asset_history::max_entries).
		asset_ops& operations() { return *m_ops; }
		asset_history& history() { return *m_history; }
		// One undo for the person: the newest of the database's fixes (Problems) and the file operations, by the order they were done in.
		std::optional<std::string> next_undo_label() const;
		asset_fix_result undo_last();
		// Takes back the operation `operation_id` if it is the newest one (the report window's Undo): refused otherwise.
		asset_fix_result undo_operation(const std::string& operation_id);
		// The asset browser's Rename, Move and Delete windows run through these: the commands' own path as a person's call.
		asset_operation_ports operation_ports();

	private:
		asset_command_sources command_sources();
		// Reads start_scene and input_actions of the manifest file again into `manifest_data`.
		void refresh_manifest();
		// The newest undo is an operation's, not a fix's.
		bool next_undo_is_operation() const;

		ports m_ports;
		std::unique_ptr<asset_database> m_database;
		// Reads the database, so it goes first.
		std::unique_ptr<picker::database_catalog> m_catalog;
		// After the database, so it detaches before the database goes.
		std::unique_ptr<asset_index_link> m_index;
		std::unique_ptr<asset_problem_publisher> m_problems;
		std::unique_ptr<asset_ops> m_ops;
		std::unique_ptr<asset_history> m_history;
	};
}
