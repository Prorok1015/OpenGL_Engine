#include "edt_level_disk_banner.h"
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_dialog_service.h>
#include <cstddef>
#include <format>
#include <utility>
#include <vector>

namespace
{
	constexpr const char* k_reload_option = "Reload (drop my changes)";
}

edt::level_disk_banner::level_disk_banner(scene_document& state, level_controller& level, dialog_service& dialogs)
	: m_state(state)
	, m_level(level)
	, m_dialogs(dialogs)
{
}

bool edt::level_disk_banner::visible() const
{
	return m_state.changed_on_disk() && m_dismissed_serial != m_state.disk_change_serial();
}

bool edt::level_disk_banner::reload_now()
{
	const bool reloaded = m_level.reload_from_disk();
	m_error = reloaded ? std::string{} : std::string{ "The level was not reloaded: Problems and the log say why." };
	return reloaded;
}

void edt::level_disk_banner::reload(std::function<void(bool reloaded)> done)
{
	if (!m_state.is_dirty()) {
		const bool reloaded = reload_now();
		if (done) done(reloaded);
		return;
	}
	m_dialogs.choose("Reload the level", "Reloading reads the level file again: your unsaved changes of the level are lost.",
		{ k_reload_option, "Cancel" },
		[this, done = std::move(done), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock()) return;
			const bool reloaded = choice && *choice == 0 && reload_now();
			if (done) done(reloaded);
		});
}

void edt::level_disk_banner::keep_mine()
{
	m_dismissed_serial = m_state.disk_change_serial();
	m_error.clear();
}

edt::save_result edt::level_disk_banner::overwrite()
{
	save_result written = m_level.overwrite_disk_change();
	m_error = written.saved ? std::string{} : written.error;
	return written;
}

void edt::level_disk_banner::ask_save(std::function<void(bool resolved)> done)
{
	m_dialogs.choose("Level file changed on disk",
		std::format("'{}' was changed on disk since the editor read it.\n\nReload reads the file again and drops your unsaved changes "
			"of the level. Overwrite replaces the file with your version.", m_state.level_tag.view()),
		{ k_reload_option, "Overwrite the file", "Cancel" },
		[this, done = std::move(done), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock()) return;
			bool resolved = false;
			if (choice && *choice == 0) {
				resolved = reload_now();
			} else if (choice && *choice == 1) {
				resolved = overwrite().saved;
			}
			if (done) done(resolved);
		});
}
