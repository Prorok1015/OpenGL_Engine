#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace edt
{
	// Tells the headless smoke scripts (tools/run_headless.sh, `drag_item`) where an item is on the screen: one log line
	// per item, only when its rectangle changed, so the log does not grow by a line per frame. Off unless
	// editor/ui_probe is set; the smoke run is the only one that sets it.
	class ui_probe
	{
	public:
		static constexpr const char* k_category = "Editor/UiProbe";
		using sink = std::function<void(const std::string& line)>;

		// Null sink: the editor log.
		explicit ui_probe(bool enabled, sink out = {});

		[[nodiscard]] bool enabled() const { return m_enabled; }
		// Screen coordinates of the item's rectangle (ImGui::GetItemRectMin/Max).
		void report(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y);

		// "item <area> '<name>' center <x> <y> size <w> <h>": what run_headless.sh greps for.
		static std::string format(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y);

	private:
		bool m_enabled;
		sink m_out;
		std::unordered_map<std::string, std::string> m_last_line;
	};
}
