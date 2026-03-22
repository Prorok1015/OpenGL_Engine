#include "edt_console_panel.h"
#include <imgui.h>
#include <algorithm>

namespace edt
{
	console_panel::console_panel()
		: panel_base("console", "Console")
	{
	}

	void console_panel::add_log(log_level level, std::string message)
	{
		std::lock_guard lock(m_mutex);
		if (m_logs.size() >= max_log_entries)
			m_logs.erase(m_logs.begin(), m_logs.begin() + (max_log_entries / 4));
		m_logs.push_back({level, std::move(message)});
	}

	void console_panel::clear()
	{
		std::lock_guard lock(m_mutex);
		m_logs.clear();
	}

	std::size_t console_panel::get_error_count() const
	{
		std::lock_guard lock(m_mutex);
		return std::count_if(m_logs.begin(), m_logs.end(),
			[](const log_entry& e) { return e.level == log_level::error; });
	}

	std::size_t console_panel::get_warning_count() const
	{
		std::lock_guard lock(m_mutex);
		return std::count_if(m_logs.begin(), m_logs.end(),
			[](const log_entry& e) { return e.level == log_level::warning; });
	}

	void console_panel::on_render()
	{
		if (ImGui::Button("Clear")) clear();
		ImGui::SameLine();
		ImGui::Checkbox("Info", &m_show_info); ImGui::SameLine();
		ImGui::Checkbox("Warn", &m_show_warnings); ImGui::SameLine();
		ImGui::Checkbox("Error", &m_show_errors);
		ImGui::Separator();

		std::vector<log_entry> logs_copy;
		{
			std::lock_guard lock(m_mutex);
			logs_copy = m_logs;
		}
		ImGui::BeginChild("log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
		for (const auto& entry : logs_copy) {
			if (entry.level == log_level::info    && !m_show_info)     continue;
			if (entry.level == log_level::warning && !m_show_warnings) continue;
			if (entry.level == log_level::error   && !m_show_errors)   continue;

			ImVec4 color;
			switch (entry.level) {
				case log_level::info:    color = {1.f, 1.f, 1.f, 1.f}; break;
				case log_level::warning: color = {1.f, 0.8f, 0.f, 1.f}; break;
				case log_level::error:   color = {1.f, 0.3f, 0.3f, 1.f}; break;
			}
			ImGui::TextColored(color, "%s", entry.message.c_str());
		}
		if (m_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(1.f);
		ImGui::EndChild();
	}
}
