#pragma once
#include "edt_panel_base.h"
#include "edt_asset_layers.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	class asset_browser_panel : public panel_base
	{
	public:
		asset_browser_panel();

		// Physical roots behind res://, lowest layer first, top last (as the resource system mounts them).
		void set_layers(std::vector<std::filesystem::path> layers);

		// Callback fired on double-click on a file entry.
		void set_on_file_double_clicked(std::function<void(const std::filesystem::path&)> cb) {
			m_on_file_double_clicked = std::move(cb);
		}

	protected:
		void on_render() override;

	private:
		// Non-recursive merged scan of m_current_rel into m_entries.
		void refresh_entries();
		// Recursive directory tree for the left panel; rel_dir is relative to res://.
		void draw_dir_tree(const std::filesystem::path& rel_dir, int depth = 0);
		// Navigate the right panel to rel_dir (relative to res://).
		void navigate_to(const std::filesystem::path& rel_dir);
		// Short icon prefix for a file based on extension.
		const char* get_file_icon(const std::filesystem::path& path) const;

		std::vector<std::filesystem::path> m_layers;
		std::filesystem::path m_current_rel;
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
