#pragma once
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// What the World Systems panel shows and does (#549), without ImGui: the list and every edit are the world.systems.*
// commands run through `invoke` as the person's calls, so an edit is one undo step exactly as the agent's is. The
// list is read again when the document or the active world changed since the last read. Main thread only.
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
			// Registered in the system registry; a name that is not is kept in the list and marked.
			bool known = true;
			std::string phase;
			bool run_in_edit = false;
		};

		world_systems_model(editor_events& events, ports ports);

		void refresh();
		// Reads the list again only when something changed since the last read.
		void refresh_if_changed();

		const std::string& world() const { return m_world; }
		const std::vector<item>& systems() const { return m_systems; }
		// The registered systems that are not in the list yet.
		const std::vector<item>& available() const { return m_available; }
		bool locked() const { return m_ports.playing && m_ports.playing(); }
		// The last refusal or failure of a command, empty when the last one went through.
		const std::string& error() const { return m_error; }

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
		bool m_dirty = true;
		connection m_document;
		connection m_active_world;
	};
}
