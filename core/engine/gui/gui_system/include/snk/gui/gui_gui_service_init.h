#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <vector>

namespace res { class resource_system; }
namespace desc { class desc_system; }

namespace engine::gui {
	// The shipped UI font (fonts/NotoSans-Regular.ttf) from engine:// (resource_system::engine_file): a project file at
	// the same res:// path is never handed to the font parser (#929). Empty, logged, when it is absent or cannot be read:
	// the backend keeps ImGui's built-in font.
	SNK_API std::vector<std::byte> load_ui_font_bytes(res::resource_system& resources);
	// The gui.theme preset type (#1109); gui_init/gui_term do it, a test that loads presets calls them itself.
	SNK_API void register_gui_descs(desc::desc_system& descs);
	// Whether gui/theme names a preset; otherwise the editor may apply its user's own choice.
	SNK_API bool theme_set_by_config();
	SNK_API void unregister_gui_descs(desc::desc_system& descs);
	void gui_init(ds::app_data_storage& data);
	void gui_term(ds::app_data_storage& data);
}