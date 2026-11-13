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
		std::string m_error;
		bool m_focus = false;
	};
}
