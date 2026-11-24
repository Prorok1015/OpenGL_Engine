#include "edt_level_disk_banner.h"
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document.h>
#include <cstddef>
#include <format>
#include <utility>

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

std::optional<std::string> edt::level_disk_banner::blocked_reason() const
{
	if (m_state.edits_locked()) return std::string{ "stop Play first" };
	return m_level.write_blocker ? m_level.write_blocker() : std::nullopt;
}

bool edt::level_disk_banner::run(action what)
{
	switch (what) {
	case action::reload: {
		const std::string why = m_level.reload_from_disk();
		m_error = why.empty() ? std::string{} : "The level was not reloaded: " + why;
		return why.empty();
	}
	case action::overwrite: {
		const save_result written = m_level.overwrite_disk_change();
		m_error = written.saved ? std::string{} : "The level file was not overwritten: " + written.error;
		return written.saved;
	}
	case action::keep_mine:
		m_dismissed_serial = m_state.disk_change_serial();
		m_error.clear();
		return true;
	}
	return false;
}

void edt::level_disk_banner::run_pending()
{
	std::vector<queued> now = std::exchange(m_pending, {});
	for (queued& item : now) {
		const bool ok = run(item.what);
		if (item.done) item.done(ok);
	}
}

void edt::level_disk_banner::reload(std::function<void(bool reloaded)> done)
{
	if (!m_state.is_dirty()) {
		m_pending.push_back({ action::reload, std::move(done) });
		return;
	}
	m_dialogs.choose("Reload the level", "Reloading reads the level file again: your unsaved changes of the level are lost.",
		{ k_reload_option, "Cancel" },
		[this, done = std::move(done), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock()) return;
			if (choice && *choice == 0) {
				m_pending.push_back({ action::reload, done });
			} else if (done) {
				done(false);
			}
		});
}

void edt::level_disk_banner::keep_mine()
{
	m_pending.push_back({ action::keep_mine, {} });
}

void edt::level_disk_banner::overwrite(std::function<void(bool written)> done)
{
	m_pending.push_back({ action::overwrite, std::move(done) });
}

void edt::level_disk_banner::ask_save(std::function<void(bool resolved)> done)
{
	m_dialogs.choose("Level file changed on disk",
		std::format("'{}' was changed on disk since the editor read it.\n\nReload reads the file again and drops your unsaved changes "
			"of the level. Overwrite replaces the file with your version.", m_state.level_tag.view()),
		{ k_reload_option, "Overwrite the file", "Cancel" },
		[this, done = std::move(done), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock()) return;
			if (choice && *choice == 0) {
				m_pending.push_back({ action::reload, done });
			} else if (choice && *choice == 1) {
				m_pending.push_back({ action::overwrite, done });
			} else if (done) {
				done(false);
			}
		});
}
