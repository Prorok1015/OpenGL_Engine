#pragma once
#include "edt_code_tree.h"
#include <snk/resource/res_system.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace edt
{
	// The "Code" tree of the Asset Browser (#450): the files of the project's code/ folder as code:// resources.
	// Double-click opens a file in the system's program; the context menu makes, renames and deletes files through
	// code_actions (a delete asks first). The tree follows the folder: it re-reads after a change reported by the
	// code:// watcher, whether the editor, an IDE or an agent made it.
	class code_section
	{
	public:
		code_section(res::resource_system& res, code_actions actions);
		~code_section();
		code_section(const code_section&) = delete;
		code_section& operator=(const code_section&) = delete;

		// Draws the tree node and, whatever its state, the dialogs it opened.
		void render();

		// A change was reported (also what the subscription calls): the next frame lists the folders again.
		void mark_changed() { m_listings.clear(); }

	private:
		enum class dialog_kind { none, new_file, rename_file, delete_file };

		void draw_folder(const std::string& relative);
		void draw_entry_menu(const code_entry& entry);
		void draw_dialogs();
		void open_dialog(dialog_kind kind, std::string target, std::string initial_text);
		void report(const res::file_operation& result);
		const std::vector<code_entry>& listing(const std::string& relative);

		res::resource_system& m_res;
		code_actions m_actions;
		std::unordered_map<std::string, std::vector<code_entry>> m_listings;

		dialog_kind m_dialog = dialog_kind::none;
		bool m_open_requested = false;
		std::string m_target;
		std::string m_text;
		std::string m_error;
	};
}
