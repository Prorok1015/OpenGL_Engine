#pragma once
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The World Systems panel's logic without ImGui (#549). Edits are world.systems.* commands run as the person's
// calls, so each is one undo step like the agent's. Main thread only.
namespace edt
{
	class world_systems_model
	{
	public:
		struct ports
		{
			// A command of the registry, as a person's call (origin ui).
			std::function<cmd::result(std::string_view id, boost::json::value args)> invoke;
			// Play runs: the commands are refused (busy), so the panel does not offer them.
			std::function<bool()> playing;
		};

		struct item
		{
			std::string name;
			// A name that is no longer registered stays in the list.
			bool known = true;
			std::string phase;
			bool run_in_edit = false;
		};

		world_systems_model(editor_events& events, ports ports);

		void refresh();
		// Only when the document or the active world changed since the last read.
		void refresh_if_changed();

		const std::string& world() const { return m_world; }
		const std::vector<item>& systems() const { return m_systems; }
		// The registered systems that are not in the list yet.
		const std::vector<item>& available() const { return m_available; }
		bool locked() const { return m_ports.playing && m_ports.playing(); }
		// A command's refusal, else why the list could not be read (no level yet); empty when neither.
		const std::string& error() const { return m_error.empty() ? m_list_error : m_error; }

		bool add(const std::string& name);
		bool remove(const std::string& name);
		bool move(const std::string& name, std::size_t to);

	private:
		bool run(std::string_view id, boost::json::object args);

		editor_events& m_events;
		ports m_ports;
		std::string m_world;
		std::vector<item> m_systems;
		std::vector<item> m_available;
		std::string m_error;
		std::string m_list_error;
		bool m_dirty = true;
		connection m_document;
		connection m_active_world;
	};
}
