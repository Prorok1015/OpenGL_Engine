#pragma once
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace cmd { class command_registry; }

namespace edt
{
	class agent_component;
	class agent_policy_model;
	class agents_panel;
	class editor_context;
	class editor_layer;

	// What a person sees of agents (#449): the agents window, `agent.settings.open`, the status bar slot, the notice of a project
	// policy waiting for approval and the last actions an agent made. Nothing in it is a command an agent could call: the buttons
	// call the policy model directly. It reads the access and the server of `agent_component`, which must outlive it and is
	// declared before it in editor_system; both may be null (after shutdown()), nothing is drawn then.
	class agent_ui_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_layer& layer;
			cmd::command_registry& commands;
			agent_component& agent;
			bool headless = false;
		};

		explicit agent_ui_component(ports from);
		~agent_ui_component();
		agent_ui_component(const agent_ui_component&) = delete;
		agent_ui_component& operator=(const agent_ui_component&) = delete;

		// The agents window (View menu order: where the caller puts it) and the action that opens it.
		void install(std::vector<registration>& out);
		// The slot in the menu bar, left of the other status items. Nothing, and no ImGui call, unless the server is active or a
		// project policy waits.
		void draw_status();

	private:
		void open_panel();
		void draw_policy_notice();

		struct recent_action
		{
			std::string command;
			std::string target;
			std::string session;
			std::chrono::steady_clock::time_point at;
		};

		ports m_ports;
		std::unique_ptr<agent_policy_model> m_model;
		std::shared_ptr<agents_panel> m_panel;
		std::deque<recent_action> m_recent_actions;
		// The pending loosenings (as text) the person sent away with "Later"; empty = not dismissed. A different set shows the notice again.
		std::string m_notice_dismissed_key;
		std::chrono::steady_clock::time_point m_policy_refreshed{};
		// Last: the listener of editor_events::agent_action goes first.
		std::vector<connection> m_connections;
	};
}
