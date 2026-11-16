#pragma once
#include <snk/gui/gui_layer_interface.h>
#include <snk/gui/gui_theme.h>
#include <snk/gui/gui_theme_desc.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_binary.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/common/snk_api.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace res { class resource_system; }

namespace gui
{
	// The style a preset describes, or nullopt with `problem` saying why it cannot be used.
	SNK_API std::optional<theme_style> theme_style_of(const theme_desc& preset, std::string& problem);

	// Loads a theme preset and its fonts through the resource system without waiting on the main thread, then hands
	// the result to `apply` (the GL backend rebuilds its atlas and style at the next frame). A preset that cannot be
	// used is a WARN and the fallback preset is loaded instead; a font that cannot be used is a WARN and the UI font
	// (Noto) takes its place, so nothing a preset says can stop the editor.
	class SNK_API theme_service
	{
	public:
		struct theme {
			theme_style style;
			std::vector<std::byte> text_font;
			std::vector<std::byte> icon_font;
		};
		using apply_function = std::function<void(theme)>;

		theme_service(res::resource_system& resources, res::tag fallback, apply_function apply);

		// The load of an earlier select() is dropped.
		void select(res::tag preset);
		// Advances the loads; called once a frame (theme_service::layer).
		void poll();
		// The preset asked for last, after a fallback the fallback.
		const res::tag& selected() const { return m_selected; }
		bool loading() const { return m_stage != stage::idle; }

		// Calls poll() every frame from the GUI system's layers.
		class layer final : public layer_interface
		{
		public:
			explicit layer(theme_service& service) : m_service(service) {}
			void on_update(std::chrono::duration<float>) override { m_service.poll(); }
		private:
			theme_service& m_service;
		};

	private:
		enum class stage { idle, preset, fonts };

		void poll_preset();
		void poll_fonts();
		void give_up(std::string_view why);
		res::res_handle<res::binary_resource> request_font(const res::tag& tag);

		res::resource_system& m_resources;
		res::tag m_fallback;
		res::tag m_ui_font;
		res::tag m_icon_font;
		apply_function m_apply;

		stage m_stage = stage::idle;
		res::tag m_selected;
		res::res_handle<theme_desc> m_preset;
		theme_style m_style;
		res::tag m_text_font_tag;
		bool m_wants_icons = false;
		res::res_handle<res::binary_resource> m_text_font;
		res::res_handle<res::binary_resource> m_icons;
	};
}
