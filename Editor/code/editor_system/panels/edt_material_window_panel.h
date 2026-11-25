#pragma once
#include "edt_data_document_panel.h"
#include "edt_material_preview.h"
#include "edt_material_window_model.h"
#include <string>

namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace inp { class input_router; }

namespace edt
{
	// What the material window draws its preview with; a null one: no image (tests), no orbit input.
	struct material_preview_view
	{
		rnd::render_system* render = nullptr;
		gui::gui_system* gui = nullptr;
		inp::input_router* input = nullptr;
	};

	// The window of a material_desc data document (#1196): the fields, Save, banners and status of data_document_panel,
	// above them the live preview (material_preview: a shape, the sky or not, an orbit camera), and under them the Defines
	// section: a checkbox per define of material_window_model, with where it comes from, and a name typed in to add one.
	// The shader's errors come later (#1196 PR 3).
	class material_window_panel final : public data_document_panel
	{
	public:
		material_window_panel(data_document& document, editor_services& services, document_id id, bool quiet = false,
			material_preview_view view = {});

		material_window_model& model() { return m_model; }
		material_preview& preview() { return m_preview; }

	protected:
		void on_hidden() override;
		void draw_before_fields() override;
		void draw_after_fields() override;

	private:
		void draw_add_define();
		void draw_preview_image();

		material_window_model m_model;
		material_preview m_preview;
		material_preview_view m_view;
		char m_new_define[128] = {};
		// Why the last name typed in was not added; empty: none.
		std::string m_add_error;
	};
}
