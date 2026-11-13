#pragma once
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_panel_base.h>
#include <functional>
#include <string>

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
		// The message under the Save button (what the last save said); empty clears it.
		void set_error(std::string error) { m_error = std::move(error); }
		// The window comes to the front at its next frame.
		void request_focus() { m_focus = true; }
		document_id id() const { return m_id; }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		data_document& m_document;
		editor_services& m_services;
		document_id m_id;
		std::function<void()> m_on_close;
		std::function<void()> m_on_save;
		std::function<void()> m_on_reload;
		std::function<void()> m_on_overwrite;
		bool m_banner_dismissed = false;
		std::string m_error;
		bool m_focus = false;
	};
}
