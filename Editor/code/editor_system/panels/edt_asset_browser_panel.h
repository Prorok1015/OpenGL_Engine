#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_asset_layers.h"
#include "edt_asset_usages.h"
#include "edt_ui_probe.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace edt
{
	class code_section;

	class asset_browser_panel : public panel_base
	{
	public:
		asset_browser_panel();

		// Physical roots behind res://, lowest layer first, top last (as the resource system mounts them).
		void set_layers(std::vector<std::filesystem::path> layers);

		// The Code tree under the res:// tree; none when the project has no code/ folder (nothing is drawn then).
		void set_code_section(std::shared_ptr<code_section> section);

		// Reports the screen rectangle of every entry (headless smoke, editor/ui_probe); null: nothing.
		void set_ui_probe(std::shared_ptr<ui_probe> probe) { m_ui_probe = std::move(probe); }

		// "Used by" of the file picked by a click (#774, EPIC-57 section 6.3): its usages as saved on the disk. Without a
		// source the section is not drawn.
		void set_usages(usage_list::report_source report, std::function<std::uint64_t()> revision)
		{
			m_usages.set_source(std::move(report), std::move(revision));
		}

		// Callback fired on double-click on a file entry; the argument is relative to res:// (layer independent).
		void set_on_file_double_clicked(std::function<void(const std::filesystem::path&)> cb) {
			m_on_file_double_clicked = std::move(cb);
		}

	protected:
		void on_render() override;
		// Closed, collapsed or in a hidden tab: nothing is drawn, so the probe's items are not on the screen.
		void on_hidden() override;

	private:
		// Non-recursive merged scan of m_current_rel into m_entries.
		void refresh_entries();
		// Recursive directory tree for the left panel; rel_dir is relative to res://.
		void draw_dir_tree(const std::filesystem::path& rel_dir, int depth = 0);
		// Navigate the right panel to rel_dir (relative to res://).
		void navigate_to(const std::filesystem::path& rel_dir);
		// The "Used by" section of the picked file, under the list.
		void draw_usages();
		// Short icon prefix for a file based on extension.
		const char* get_file_icon(const std::filesystem::path& path) const;

		std::vector<std::filesystem::path> m_layers;
		std::shared_ptr<code_section> m_code_section;
		std::shared_ptr<ui_probe> m_ui_probe;
		std::filesystem::path m_current_rel;
		// The file picked by a click, relative to res://; empty: none.
		std::filesystem::path m_selected_rel;
		usage_list m_usages;
		char m_search[128] = {};

		struct entry {
			layer_entry src;
			std::string label; // pre-built "icon filename"
		};
		std::vector<entry> m_entries;
		bool m_needs_refresh = true;
		std::function<void(const std::filesystem::path&)> m_on_file_double_clicked;
	};
}
