#pragma once
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class dialog_service;
	class level_controller;
	class scene_document;

	// The banner over the viewport while the level file changed on disk (#1188), without ImGui: whether it shows and what its
	// buttons do (the viewport panel draws it). The question of a person's Save of a flagged level is asked through it too.
	// Reload and Overwrite rebuild the level or write it, so a click or an answer only queues them: run_pending carries them
	// out once a frame, outside the UI, where no panel holds a registry of the worlds. An action is for the level and the
	// outside version it was asked about: when the level or the version is another by then, it is dropped.
	class level_disk_banner
	{
	public:
		static constexpr std::string_view k_text = "The level file changed on disk.";

		level_disk_banner(scene_document& state, level_controller& level, dialog_service& dialogs);
		level_disk_banner(const level_disk_banner&) = delete;
		level_disk_banner& operator=(const level_disk_banner&) = delete;

		// The level is flagged (scene_document::changed_on_disk) and Keep mine did not put off this outside version.
		bool visible() const;
		// Why Reload and Overwrite cannot run now (Play, a level loading, a migration, a detached level); nullopt: they can.
		std::optional<std::string> blocked_reason() const;
		// Reload: asks first while the level has unsaved changes (they are lost), then the level takes its file; bytes that do
		// not load leave the level as it is and say why in Problems. `done(true)` once the level took the file.
		void reload(std::function<void(bool reloaded)> done = {});
		// Keep mine: the banner goes until the next outside change; the flag stays, so Save still asks.
		void keep_mine();
		// Overwrite: the editor's level over the file, unless the file changed again meanwhile (flagged anew: the banner is back).
		void overwrite(std::function<void(bool written)> done = {});
		// What the last action of the banner failed with; empty: nothing.
		const std::string& error() const { return m_error; }
		// A person's Save of a flagged level: Reload / Overwrite / Cancel. `done(true)` once the level and its file agree again.
		void ask_save(std::function<void(bool resolved)> done);

		bool has_pending() const { return !m_pending.empty(); }
		// Carries out the queued actions, each `done` after its own.
		void run_pending();

	private:
		enum class action : std::uint8_t { reload, overwrite, keep_mine };
		// What an action was asked about: the level file and its outside version (scene_document::disk_change_serial).
		struct asked_about
		{
			res::tag level;
			std::uint64_t serial = 0;
		};
		struct queued
		{
			action what = action::reload;
			asked_about about;
			std::function<void(bool)> done;
		};

		asked_about now() const;
		bool run(const queued& item);

		scene_document& m_state;
		level_controller& m_level;
		dialog_service& m_dialogs;
		std::optional<std::uint64_t> m_dismissed_serial;
		std::string m_error;
		std::vector<queued> m_pending;
		// The dialog queue can outlive the banner: an answer after that does nothing.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
