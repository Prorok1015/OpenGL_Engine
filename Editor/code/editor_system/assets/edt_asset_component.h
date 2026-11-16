#pragma once
#include "edt_asset_database.h"
#include "edt_asset_index.h"
#include "edt_asset_picker.h"
#include "edt_asset_problems.h"
#include <snk/editor/edt_registration.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace desc { class desc_system; }
namespace prj { struct project_context; }
namespace res { class resource_system; }

namespace edt
{
	class asset_browser_panel;
	class editor_context;
	struct problem;

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
		};

		explicit asset_component(ports from);
		~asset_component();
		asset_component(const asset_component&) = delete;
		asset_component& operator=(const asset_component&) = delete;

		// asset.usages and the asset browser's usage list.
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
		std::optional<std::string> next_undo_label() const { return m_database->next_undo_label(); }
		asset_fix_result undo_last() { return m_database->undo_last(); }

	private:
		ports m_ports;
		std::unique_ptr<asset_database> m_database;
		// Reads the database, so it goes first.
		std::unique_ptr<picker::database_catalog> m_catalog;
		// After the database, so it detaches before the database goes.
		std::unique_ptr<asset_index_link> m_index;
		std::unique_ptr<asset_problem_publisher> m_problems;
	};
}
