#pragma once
#include <string>
// ImGuiWindowFlags is an imgui integer typedef (int). Re-declared here to keep <imgui.h>
// out of this header, which leaks into every panel (EPIC-49 US-49-4). Same-type redefinition
// is legal alongside <imgui.h>; the .cpp includes the full header for the ImGui API.
using ImGuiWindowFlags = int;

namespace edt
{
	class panel_base
	{
	public:
		panel_base(std::string_view id, std::string_view title);
		virtual ~panel_base() = default;

		void render();

		bool is_open() const { return m_open; }
		void set_open(bool open) { m_open = open; }
		const std::string& get_id() const { return m_id; }
		const std::string& get_title() const { return m_title; }

	protected:
		virtual void on_before_begin() {}
		virtual void on_render() = 0;
		// Called instead of on_render() when the panel is closed, collapsed or in a hidden tab.
		virtual void on_hidden() {}

		std::string m_id;
		std::string m_title;
		bool m_open = true;
		ImGuiWindowFlags m_flags = 0;
	};
}
