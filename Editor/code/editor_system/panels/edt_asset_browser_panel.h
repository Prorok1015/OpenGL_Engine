#pragma once
#include "edt_panel_base.h"
#include <string>
#include <vector>

namespace edt
{
	class asset_browser_panel : public panel_base
	{
	public:
		asset_browser_panel();

	protected:
		void on_render() override;

	private:
		std::vector<std::string> m_root_dirs = {"objects", "shaders", "textures", "levels", "skybox"};
		int m_selected_dir = 0;
		char m_search[128] = {};
	};
}
