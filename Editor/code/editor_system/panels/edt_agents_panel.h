#pragma once
#include "edt_panel_base.h"
#include "edt_agent_policy_model.h"
#include "edt_agent_server.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	// What the agents window shows of the server and its recent actions, and the one thing it can do to the server.
	struct agents_panel_services
	{
		// null: no agent server in this editor.
		std::function<agent_server*()> server;
		// The person's switch (editor_system::set_agent_server). False: it did not take effect (port taken).
		std::function<bool(bool enabled, std::uint16_t port)> set_server;
		// The port of the person's last choice (edt://settings/agent_server.json).
		std::function<std::uint16_t()> chosen_port;
	};

	// "MCP / Agents" (View menu, floating, closed by default, #449): the agent server on and off with its port, the agents
	// connected and a Disconnect for each, and the policy as a table (categories, commands, the git section) in two layers,
	// with the approval banner for a project file that loosens something. Draws agent_policy_model and nothing else; there is
	// no command behind any button.
	class agents_panel : public panel_base
	{
	public:
		static constexpr const char* k_id = "agents";
		static constexpr const char* k_title = "MCP / Agents";

		agents_panel(agent_policy_model& model, agents_panel_services services);

		void focus() { m_focus = true; }

	protected:
		void on_before_begin() override;
		void on_render() override;

	private:
		void draw_banner();
		void draw_review_popup();
		void draw_server();
		void draw_sessions();
		void draw_policy();
		void draw_vcs(policy_scope scope);
		void draw_footer();

		agent_policy_model& m_model;
		agents_panel_services m_services;
		policy_scope m_scope = policy_scope::project;
		cmd::agent_category m_command_category = cmd::agent_category::code_edit;
		int m_port_edit = 0;
		bool m_port_loaded = false;
		bool m_focus = false;
		std::string m_message;
		bool m_message_is_error = false;
		// The text of the git lists while they are typed: policy_layer holds the parsed lists.
		std::string m_list_text[3];
		bool m_list_loaded[3] = { false, false, false };
		policy_scope m_list_scope = policy_scope::project;
	};

	// "a, b" -> {"a", "b"}: trimmed, empty pieces dropped.
	std::vector<std::string> split_pattern_list(const std::string& text);
	std::string join_pattern_list(const std::vector<std::string>& list);
}
