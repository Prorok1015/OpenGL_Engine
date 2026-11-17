#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace edt
{
	// Tells the headless smoke scripts (tools/run_headless.sh, `drag_item`) where an item is on the screen: one log line
	// per item, only when what it says changed, so the log does not grow by a line per frame. The editor makes one only
	// with editor/ui_probe (the smoke run is the only one that sets it), so without the flag nothing is kept or written.
	//
	// The last line of an item is the truth: `item <area> '<name>' center <x> <y> size <w> <h>` while it is drawn inside
	// its window, `item <area> '<name>' hidden` once it is clipped away or not drawn any more (another folder, a filter).
	class ui_probe
	{
	public:
		static constexpr const char* k_category = "Editor/UiProbe";
		using sink = std::function<void(const std::string& line)>;

		// Null sink: the editor log.
		explicit ui_probe(sink out = {});

		// Screen coordinates of the item's rectangle (ImGui::GetItemRectMin/Max) of an item that is visible.
		void report(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y);
		// An item the window drew but clipped (ImGui::IsItemVisible() is false): its rectangle would be off the panel.
		void report_hidden(std::string_view area, std::string_view name);

		// A pass over the items of an area: the ones reported before and not in this pass become hidden at end_pass.
		void begin_pass(std::string_view area);
		void end_pass(std::string_view area);

		static std::string format(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y);
		static std::string format_hidden(std::string_view area, std::string_view name);

	private:
		struct item
		{
			std::string area;
			std::string name;
			std::string line;
			bool seen = false;
		};
		void write(item& entry, std::string line);
		item& entry_of(std::string_view area, std::string_view name);

		sink m_out;
		std::unordered_map<std::string, item> m_items;
	};
}
