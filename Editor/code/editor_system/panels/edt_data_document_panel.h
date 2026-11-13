#pragma once
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_panel_base.h>
#include "edt_schema_inspector.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace edt
{
	class data_document;
	struct editor_services;

	// The window of one data document (EPIC-56, #793): its fields from the schema (schema_ui::draw_fields over
	// data_document::edit), a Save button and what the document says about itself (the parents it inherits from, a broken
	// chain, a file changed on disk). Closing the window asks for the document to be closed (`on_close`): the editor decides,
	// it asks first when there are unsaved changes.
	class data_document_panel final : public panel_base
	{
	public:
		data_document_panel(data_document& document, editor_services& services, document_id id);

		void set_on_close(std::function<void()> on_close) { m_on_close = std::move(on_close); }
		// What the window's buttons ask the editor for: Save (a version conflict becomes a question), and the two answers of
		// the "changed on disk" banner. Unset: Save writes at once and the banner has no buttons (tests).
		void set_on_save(std::function<void()> on_save) { m_on_save = std::move(on_save); }
		void set_on_reload(std::function<void()> on_reload) { m_on_reload = std::move(on_reload); }
		void set_on_overwrite(std::function<void()> on_overwrite) { m_on_overwrite = std::move(on_overwrite); }
		// The window got the focus (its document is the one Edit > Undo acts on) or lost it to another window: `taken_by` is
		// that window's name. A menu or a dialog open over the window is neither: the focus returns to it, or the person went
		// elsewhere once they close. A window the focus goes to that is no place to work in (the dockspace host with the main
		// menu bar, a popup, a tooltip, a window that takes no navigation focus) is reported as an empty name.
		void set_on_focus(std::function<void()> on_focus) { m_on_focus = std::move(on_focus); }
		void set_on_blur(std::function<void(std::string_view taken_by)> on_blur) { m_on_blur = std::move(on_blur); }
		// The message under the Save button (what the last save said); empty clears it.
		void set_error(std::string error) { m_error = std::move(error); }
		// The window comes to the front at its next frame.
		void request_focus() { m_focus = true; }
		document_id id() const { return m_id; }
		// What the map rows of this window keep between frames (the text of a "new key" field, a refused rename). Its own,
		// not the inspector's: it is dropped when the document changes under the window (a reload, a parent that changed).
		schema_ui::map_edit_state& map_state() { return m_map_state; }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		void follow_focus();
		void lose_focus();

		data_document& m_document;
		editor_services& m_services;
		document_id m_id;
		std::function<void()> m_on_close;
		std::function<void()> m_on_save;
		std::function<void()> m_on_reload;
		std::function<void()> m_on_overwrite;
		std::function<void()> m_on_focus;
		std::function<void(std::string_view)> m_on_blur;
		bool m_focused = false;
		// The outside change the person put off with Later (data_document::disk_change_serial); a newer one shows the banner again.
		std::uint64_t m_dismissed_serial = 0;
		bool m_dismissed = false;
		std::string m_error;
		bool m_focus = false;
		schema_ui::map_edit_state m_map_state;
		std::uint64_t m_state_generation = 0;
	};
}
