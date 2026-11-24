#pragma once
#include <snk/editor/edt_document.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace edt
{
	class dialog_service;
	class level_controller;
	class scene_document;

	// The banner over the viewport while the level file changed on disk (#1188), without ImGui: whether it shows and what its
	// buttons do (the viewport panel draws it). The question of a person's Save of a flagged level is asked through it too.
	class level_disk_banner
	{
	public:
		static constexpr std::string_view k_text = "The level file changed on disk.";

		level_disk_banner(scene_document& state, level_controller& level, dialog_service& dialogs);
		level_disk_banner(const level_disk_banner&) = delete;
		level_disk_banner& operator=(const level_disk_banner&) = delete;

		// The level is flagged (scene_document::changed_on_disk) and Keep mine did not put off this outside version.
		bool visible() const;
		// Reload: asks first while the level has unsaved changes (they are lost), then the level takes its file; bytes that do
		// not load leave the level as it is and say why in Problems. `done(true)` once the level took the file.
		void reload(std::function<void(bool reloaded)> done = {});
		// Keep mine: the banner goes until the next outside change; the flag stays, so Save still asks.
		void keep_mine();
		// Overwrite: the editor's level over the file, unless the file changed again meanwhile (flagged anew: the banner is back).
		save_result overwrite();
		// What the last action of the banner failed with; empty: nothing.
		const std::string& error() const { return m_error; }
		// A person's Save of a flagged level: Reload / Overwrite / Cancel. `done(true)` once the level and its file agree again.
		void ask_save(std::function<void(bool resolved)> done);

	private:
		bool reload_now();

		scene_document& m_state;
		level_controller& m_level;
		dialog_service& m_dialogs;
		std::optional<std::uint64_t> m_dismissed_serial;
		std::string m_error;
		// The dialog queue can outlive the banner: an answer after that does nothing.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
