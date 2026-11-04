#pragma once
#include "edt_panel_base.h"
#include "edt_forge_pr_dialog_model.h"
#include "edt_git_panel_model.h"
#include "edt_history_model.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
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
	// nothing and draws nothing; the history is read only while its tab is shown. "Create PR" (#458) opens a form for
	// forge.create_pr (forge_pr_dialog_model; a draft unless the person unticks it), "Account..." the Git Account window.
	// A change of code/ or res/ (#515) or of the
	// repository (editor_events::repository_changed, #485) refreshes it.
	class git_panel final : public panel_base
	{
	public:
		git_panel(editor_events& events, git_panel_model::ports ports, dialog_service& dialogs, res::resource_system& resources);
		~git_panel() override;
		git_panel(const git_panel&) = delete;
		git_panel& operator=(const git_panel&) = delete;

		git_panel_model& model() { return m_model; }
		vcs::history_model& history() { return m_history; }
		forge_pr_dialog_model& pr_dialog() { return m_pr; }
		// What "Account..." (and the dialog's "no token" button) does: opens the Git Account window. Null: the buttons do nothing.
		void set_open_account(std::function<void()> open) { m_open_account = std::move(open); }
		// forge_service::describe_target, for the Create PR dialog; without it the dialog cannot create anything.
		void set_describe_target(std::function<forge_service::target_description(const std::string&)> describe)
		{
			m_pr.set_describe_target(std::move(describe));
		}

	protected:
		void on_render() override;
		void on_hidden() override;

	private:
		void draw_header();
		void draw_create_pr();
		void draw_pr_target(bool running);
		void draw_changes();
		void draw_commit();
		void draw_diff();
		void draw_history();
		void draw_history_filter();
		void draw_history_table(float height);
		void draw_history_details();

		git_panel_model m_model;
		vcs::history_model m_history;
		forge_pr_dialog_model m_pr;
		std::function<void()> m_open_account;
		std::uint64_t m_seen_writes = 0;
		std::uint64_t m_seen_refs = 0;
		// The selected file's diff split into lines once per history_model::diff_version, not every frame.
		std::vector<git_diff_line> m_history_diff;
		std::uint64_t m_history_diff_version = 0;
		dialog_service& m_dialogs;
		res::resource_system& m_resources;
		std::string m_new_branch;
		// The dialog service outlives the panel: a question still open when the panel goes answers into nothing.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
