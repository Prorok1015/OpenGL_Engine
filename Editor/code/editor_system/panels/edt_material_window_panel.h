#pragma once
#include "edt_data_document_panel.h"
#include "edt_material_window_model.h"
#include <string>

namespace edt
{
	// The window of a material_desc data document (#1196): the fields, Save, banners and status of data_document_panel,
	// and under them the Defines section: a checkbox per define of material_window_model, with where it comes from, and a
	// name typed in to add one. The preview and the shader's errors come later (#1196 PR 2 and 3).
	class material_window_panel final : public data_document_panel
	{
	public:
		material_window_panel(data_document& document, editor_services& services, document_id id, bool quiet = false);

		material_window_model& model() { return m_model; }

	protected:
		void draw_after_fields() override;

	private:
		void draw_add_define();

		material_window_model m_model;
		char m_new_define[128] = {};
		// Why the last name typed in was not added; empty: none.
		std::string m_add_error;
	};
}
