#pragma once
#include "edt_data_document_panel.h"
#include "edt_material_preview_content.h"
#include "edt_material_window_model.h"
#include "edt_preview_stage.h"
#include <string>

namespace edt
{
	// The window of a material_desc data document (#1196): the fields, Save, banners and status of data_document_panel,
	// above them the live preview (a preview_stage showing a material_preview_content: a shape, the sky or not, an orbit
	// camera), and under them the Defines section: a checkbox per define of material_window_model, with where it comes
	// from, and a name typed in to add one.
	// The shader's errors come later (#1196 PR 3).
	class material_window_panel final : public data_document_panel
	{
	public:
		material_window_panel(data_document& document, editor_services& services, document_id id, bool quiet = false,
			preview_view view = {});

		material_window_model& model() { return m_model; }
		preview_stage& stage() { return m_stage; }
		material_preview_content& material_content() { return m_content; }

	protected:
		void on_hidden() override;
		void draw_before_fields() override;
		void draw_after_fields() override;

	private:
		void draw_add_define();

		material_window_model m_model;
		material_preview_content m_content;
		// After the content: it is closed (and gives the content's copy up) before the content goes.
		preview_stage m_stage;
		char m_new_define[128] = {};
		// Why the last name typed in was not added; empty: none.
		std::string m_add_error;
	};
}
