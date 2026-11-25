#pragma once
#include "edt_asset_picker.h"
#include "edt_data_document_panel.h"
#include "edt_preview_stage.h"
#include "edt_texture_preview_content.h"
#include "edt_texture_window_model.h"
#include <snk/model_codec/res_tag.h>
#include <string>

namespace rnd { class render_system; }

namespace edt
{
	// "256x256 RGBA8, 1 mip level(s)" of the texture the renderer made from `tag`, or why there is none yet. Null renderer: "".
	std::string texture_info_text(rnd::render_system* render, const res::tag& tag);

	// The preview of a texture window or viewer (#1205): the Sky toggle, the image in a child resizable from its bottom edge,
	// and under it the size and format the renderer made, the source and the mip levels.
	void draw_texture_preview(preview_stage& stage, rnd::render_system* render, const res::tag& texture, const texture_source& source,
		int mip_levels);

	// The window of a texture_2d_desc data document (#1205): the fields, Save, banners and status of data_document_panel,
	// above them the preview (a plane with the texture on a preview_stage), under them the Sampling section (wrap, filters,
	// mip levels) of texture_window_model and, for inline pixels, "Use file..." with a list of the picture files.
	class texture_window_panel final : public data_document_panel
	{
	public:
		texture_window_panel(data_document& document, editor_services& services, document_id id, bool quiet = false,
			preview_view view = {});
		~texture_window_panel() override;

		texture_window_model& model() { return m_model; }
		preview_stage& stage() { return m_stage; }
		texture_preview_content& texture_content() { return m_content; }

	protected:
		void on_hidden() override;
		void draw_before_fields() override;
		void draw_after_fields() override;

	private:
		void draw_file_picker();

		texture_window_model m_model;
		rnd::render_system* m_render = nullptr;
		texture_preview_content m_content;
		// After the content: it is closed (and gives the content's copies up) before the content goes.
		preview_stage m_stage;
		std::string m_query;
		picker::page_cache m_page;
	};
}
