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
	// (Noto) takes its place (the text font for bold and mono), so nothing a preset says can stop the editor.
	class SNK_API theme_service
	{
	public:
		struct theme {
			theme_style style;
			std::vector<std::byte> text_font;
			// The UI font, merged after a text font of the person's for the glyphs that one lacks; empty otherwise.
			std::vector<std::byte> fallback_font;
			std::vector<std::byte> icon_font;
			// Empty: the text font stands in (gui::ui_face keeps their indices).
			std::vector<std::byte> bold_font;
			std::vector<std::byte> mono_font;
			// False: the fonts are the ones applied last (only colors or sizes changed), the bytes are empty and the
			// atlas stays as it is.
			bool fonts_changed = true;
		};
		using apply_function = std::function<void(theme)>;

		// Presets and fonts are read only from tags that start with one of the sources (default "engine://"), besides
		// the fallback and the UI and icon fonts, which come from resource_system::engine_file: a project's res://
		// file never reaches the font parser (#929). The editor adds edt://themes/ and edt://fonts/.
		theme_service(res::resource_system& resources, res::tag fallback, apply_function apply,
			std::vector<std::string> preset_sources = { "engine://" },
			std::vector<std::string> font_sources = { "engine://" });

		// Each source is a folder prefix ("edt://themes/"); a missing trailing '/' is added.
		void set_sources(std::vector<std::string> preset_sources, std::vector<std::string> font_sources);

		// The load of an earlier select() is dropped.
		void select(res::tag preset);
		// Shows `preset` without a file (an edit not saved yet); selected() stays. A preset that cannot be used is a
		// WARN and the look stays; nothing is shown while the preset of a select() still loads.
		void preview(const theme_desc& preset);
		// The next select() or preview() reads its fonts again: a font of the person's replaced under the same name
		// (#1128). Drops the cached bytes of every such font read so far; the UI and icon fonts are not touched.
		void forget_fonts();
		// Advances the loads; called once a frame (theme_service::layer).
		void poll();
		// The preset asked for last, after a fallback the fallback.
		const res::tag& selected() const { return m_selected; }
		bool loading() const { return m_stage != stage::idle; }
		// The style handed to `apply` last (a fallback's too); default constructed before the first.
		const theme_style& applied() const { return m_applied; }

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

		struct font_key {
			res::tag text;
			res::tag bold;
			res::tag mono;
			float px = 0.f;
			bool icons = false;
			bool operator==(const font_key&) const = default;
		};

		void poll_preset();
		void poll_fonts();
		bool start(const theme_desc& preset, std::string& problem);
		void give_up(std::string_view why);
		void drop_loads();
		bool readable(const res::tag& tag, const std::vector<std::string>& sources) const;
		static std::string sources_text(const std::vector<std::string>& sources);
		res::res_handle<res::binary_resource> request_font(const res::tag& tag);
		// A bold or mono face to read; an empty tag when the text font stands in for it.
		res::tag face_tag(const res::tag& tag, std::string_view field) const;
		std::vector<std::byte> face_bytes(const res::res_handle<res::binary_resource>& handle, const res::tag& tag,
			std::string_view field) const;

		res::resource_system& m_resources;
		res::tag m_fallback;
		res::tag m_ui_font;
		res::tag m_icon_font;
		apply_function m_apply;
		std::vector<std::string> m_preset_sources;
		std::vector<std::string> m_font_sources;

		stage m_stage = stage::idle;
		res::tag m_selected;
		res::res_handle<theme_desc> m_preset;
		theme_style m_style;
		res::tag m_text_font_tag;
		res::tag m_bold_font_tag;
		res::tag m_mono_font_tag;
		bool m_wants_icons = false;
		res::res_handle<res::binary_resource> m_text_font;
		res::res_handle<res::binary_resource> m_bold_font;
		res::res_handle<res::binary_resource> m_mono_font;
		res::res_handle<res::binary_resource> m_fallback_font;
		res::res_handle<res::binary_resource> m_icons;
		std::optional<font_key> m_applied_fonts;
		// The fonts read besides the UI and icon fonts, for forget_fonts().
		std::vector<res::tag> m_read_fonts;
		theme_style m_applied;
	};
}
