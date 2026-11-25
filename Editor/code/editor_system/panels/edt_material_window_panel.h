#pragma once
#include "edt_asset_picker.h"
#include "edt_data_document_panel.h"
#include "edt_material_preview_content.h"
#include "edt_material_window_model.h"
#include "edt_preview_stage.h"
#include "edt_texture_slot_model.h"
#include <string>
#include <utility>

namespace gui { class gui_system; }
namespace rnd { class render_system; }

namespace edt
{
	// The window of a material_desc data document (#1196): the fields, Save, banners and status of data_document_panel,
	// above them the live preview (a preview_stage showing a material_preview_content: a shape, the sky or not, an orbit
	// camera), and under them the Textures section: a row per sampler slot of texture_slot_model (a thumbnail, the reference,
	// "..." for the picker of texture descs, a drop from the Asset Browser, Clear); the Defines section: a checkbox per define
	// of material_window_model, with where it comes from, and a name typed in to add one; then the Compile errors section:
	// the renderer's problems of this material's define set (open) and of the shader's other variants (collapsed) with Go to,
	// and the define pre-check (#1196 PR 3).
	class material_window_panel final : public data_document_panel
	{
	public:
		material_window_panel(data_document& document, editor_services& services, document_id id, bool quiet = false,
			preview_view view = {});

		material_window_model& model() { return m_model; }
		preview_stage& stage() { return m_stage; }
		material_preview_content& material_content() { return m_content; }
		texture_slot_model& texture_slots() { return m_slots; }
		// Which texture desc a picture dropped on a slot stands for (null: a picture is always refused).
		void set_picture_texture(picker::picture_texture_lookup lookup) { m_picture_texture = std::move(lookup); }

	protected:
		void on_hidden() override;
		void draw_before_fields() override;
		void draw_after_fields() override;

	private:
		void draw_textures();
		void draw_texture_slot(const texture_slot& slot, const picker::catalog* catalog);
		void draw_slot_thumbnail(const std::string& texture);
		void draw_defines();
		void draw_add_define();
		void draw_compile_errors();
		void draw_compile_row(const compile_row& row, int index);

		// The renderer the preview draws with: its problems are the window's compile errors; null: none (tests).
		rnd::render_system* m_render = nullptr;
		// Draws the slots' thumbnails; null: none (tests).
		gui::gui_system* m_gui = nullptr;
		// The base panel's is private: Go to finds the file on the disk through it.
		editor_services& m_editor_services;
		material_window_model m_model;
		texture_slot_model m_slots;
		picker::picture_texture_lookup m_picture_texture;
		material_preview_content m_content;
		// After the content: it is closed (and gives the content's copy up) before the content goes.
		preview_stage m_stage;
		char m_new_define[128] = {};
		// Why the last name typed in was not added; empty: none.
		std::string m_add_error;
		// Why the last Go to opened nothing; empty: none.
		std::string m_go_to_error;
	};
}
