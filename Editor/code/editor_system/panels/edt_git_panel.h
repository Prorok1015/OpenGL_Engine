#pragma once
#include "edt_panel_base.h"
#include "edt_git_panel_model.h"
#include "edt_history_model.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace res
{
	class resource_system;
}

namespace edt
{
	class dialog_service;

	// View > Git (#456): the repository's changes, commit, branches and the diff of a file (tab Changes), and the
	// history with its graph and the files of a commit (tab History, #457); it only draws git_panel_model and
	// vcs::history_model, which run everything through the vcs.* commands. Closed by default: while closed it reads
	// nothing and draws nothing; the history is read only while its tab is shown. A change of code/ refreshes it (the
	// repository outside code/ through Refresh, #485).
	class git_panel final : public panel_base
	{
	public:
		git_panel(editor_events& events, git_panel_model::ports ports, dialog_service& dialogs, res::resource_system& resources);
		~git_panel() override;
		git_panel(const git_panel&) = delete;
		git_panel& operator=(const git_panel&) = delete;

		git_panel_model& model() { return m_model; }
		vcs::history_model& history() { return m_history; }

	protected:
		void on_render() override;
		void on_hidden() override;

	private:
		void draw_header();
		void draw_changes();
		void draw_commit();
		void draw_diff();
		void draw_history();
		void draw_history_filter();
		void draw_history_table(float height);
		void draw_history_details();

		git_panel_model m_model;
		vcs::history_model m_history;
		std::uint64_t m_seen_writes = 0;
		// The selected file's diff split into lines once, not every frame; the key says what it was split from.
		std::vector<git_diff_line> m_history_diff;
		std::string m_history_diff_key;
		dialog_service& m_dialogs;
		res::resource_system& m_resources;
		std::string m_new_branch;
		// The dialog service outlives the panel: a question still open when the panel goes answers into nothing.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
