#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_preview_stage.h"
#include "edt_texture_preview_content.h"
#include "edt_texture_window_model.h"
#include <snk/model_codec/res_tag.h>
#include <functional>
#include <string>
#include <utility>

namespace rnd { class render_system; }

namespace edt
{
	struct editor_services;

	// A texture shown read-only (#1205): a picture that has no texture desc yet, or a texture desc of a lower layer (the
	// engine's, a package's) that is no data document of the project. The preview and what the renderer made of it; for a
	// picture of the project, "Create Texture" (set_create).
	class texture_viewer_panel final : public panel_base
	{
	public:
		texture_viewer_panel(std::string id, res::tag source, texture_preview_source preview, texture_source shown, int mip_levels,
			editor_services& services, preview_view view = {});
		~texture_viewer_panel() override;

		// The window was closed (its X): the owner lets it go.
		void set_on_close(std::function<void()> on_close) { m_on_close = std::move(on_close); }
		// A picture's "Create Texture"; `refusal` non-empty: the button is off and says why. Unset: no button.
		void set_create(std::function<void()> create, std::string refusal);
		void set_refusal(std::string refusal) { m_refusal = std::move(refusal); }
		// A picture whose texture is there already: "Open Texture" in place of Create Texture. Empty: none.
		void set_existing(std::function<void()> open) { m_open_existing = std::move(open); }
		bool has_existing() const { return static_cast<bool>(m_open_existing); }
		// What the last Create said when it failed; empty clears it.
		void set_error(std::string error) { m_error = std::move(error); }
		void request_focus() { m_focus = true; }

		const res::tag& source() const { return m_source; }
		preview_stage& stage() { return m_stage; }
		texture_preview_content& texture_content() { return m_content; }
		const std::string& refusal() const { return m_refusal; }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		res::tag m_source;
		texture_source m_shown;
		int m_mip_levels = 1;
		rnd::render_system* m_render = nullptr;
		texture_preview_content m_content;
		// After the content: it is closed (and gives the content's copies up) before the content goes.
		preview_stage m_stage;
		std::function<void()> m_on_close;
		std::function<void()> m_create;
		std::function<void()> m_open_existing;
		std::string m_refusal;
		std::string m_error;
		bool m_focus = false;
	};
}
