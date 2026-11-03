#pragma once
#include "edt_panel_base.h"
#include "edt_git_panel_model.h"
#include <string>

namespace res
{
	class resource_system;
}

namespace edt
{
	class dialog_service;

	// View > Git (#456): the repository's changes, commit, branches and the diff of a file; it only draws
	// git_panel_model, which runs everything through the vcs.* commands. Closed by default: while closed it reads
	// nothing and draws nothing. A change of code/ refreshes it (the repository outside code/ through Refresh, #485).
	class git_panel final : public panel_base
	{
	public:
		git_panel(editor_events& events, git_panel_model::ports ports, dialog_service& dialogs, res::resource_system& resources);
		~git_panel() override;
		git_panel(const git_panel&) = delete;
		git_panel& operator=(const git_panel&) = delete;

		git_panel_model& model() { return m_model; }

	protected:
		void on_render() override;
		void on_hidden() override;

	private:
		void draw_header();
		void draw_changes();
		void draw_commit();
		void draw_diff();

		git_panel_model m_model;
		dialog_service& m_dialogs;
		res::resource_system& m_resources;
		std::string m_message;
		std::string m_new_branch;
	};
}
