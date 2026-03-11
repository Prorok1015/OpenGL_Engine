#pragma once
#include <string>
#include <imgui.h>

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
		virtual void on_render() = 0;

		std::string m_id;
		std::string m_title;
		bool m_open = true;
		ImGuiWindowFlags m_flags = 0;
	};
}
