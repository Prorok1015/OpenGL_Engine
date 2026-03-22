#pragma once
#include "edt_panel_base.h"
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

		// Set the physical base path for the resource directory.
		void set_base_path(std::filesystem::path base_path);

		// Callback fired on double-click on a file entry.
		void set_on_file_double_clicked(std::function<void(const std::filesystem::path&)> cb) {
			m_on_file_double_clicked = std::move(cb);
		}

	protected:
		void on_render() override;

	private:
		// Non-recursive scan of m_current_dir into m_entries.
		void refresh_entries();
		// Recursive directory tree for the left panel.
		void draw_dir_tree(const std::filesystem::path& dir, int depth = 0);
		// Navigate the right panel to dir.
		void navigate_to(const std::filesystem::path& dir);
		// Short icon prefix for a file based on extension.
		const char* get_file_icon(const std::filesystem::path& path) const;

		std::filesystem::path m_base_path;
		std::filesystem::path m_current_dir;
		char m_search[128] = {};

		struct entry {
			std::filesystem::path path;
			bool        is_dir = false;
			std::string label; // pre-built "icon filename"
		};
		std::vector<entry> m_entries;
		bool m_needs_refresh = true;
		std::function<void(const std::filesystem::path&)> m_on_file_double_clicked;
	};
}
