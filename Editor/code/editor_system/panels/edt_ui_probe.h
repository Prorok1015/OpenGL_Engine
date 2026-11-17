#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace edt
{
	// Tells the headless smoke scripts (tools/run_headless.sh, `drag_item`) where an item is on the screen: one log line
	// per item, only when what it says changed, so the log does not grow by a line per frame. The editor makes one only
	// with editor/ui_probe (the smoke run is the only one that sets it), so without the flag nothing is kept or written.
	//
	// The last line of an item is the truth: `item <area> '<name>' center <x> <y> size <w> <h>` while it is drawn inside
	// its window, `item <area> '<name>' hidden` once it is clipped away or not drawn any more (another folder, a
	// filter), `item <area> '<name>' ambiguous` when a pass drew two items of that name (the script refuses to guess).
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
		// Inside a pass the lines are written at end_pass, so an item reported twice is one `ambiguous` line.
		void begin_pass(std::string_view area);
		void end_pass(std::string_view area);

		// begin_pass now, end_pass when it goes out of scope: an early return of a draw function still ends its passes.
		class scoped_pass
		{
		public:
			scoped_pass(ui_probe* probe, std::initializer_list<std::string_view> areas);
			~scoped_pass();
			scoped_pass(const scoped_pass&) = delete;
			scoped_pass& operator=(const scoped_pass&) = delete;

		private:
			ui_probe* m_probe;
			std::vector<std::string_view> m_areas;
		};

		static std::string format(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y);
		static std::string format_hidden(std::string_view area, std::string_view name);
		static std::string format_ambiguous(std::string_view area, std::string_view name);

	private:
		struct item
		{
			std::string area;
			std::string name;
			std::string line;
			std::string pending;
			bool seen = false;
		};
		void write(item& entry, std::string line);
		item& entry_of(std::string_view area, std::string_view name);
		void take(item& entry, std::string line);

		sink m_out;
		std::unordered_map<std::string, item> m_items;
		std::unordered_set<std::string> m_open_passes;
	};
}
