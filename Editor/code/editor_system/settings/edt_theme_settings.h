#pragma once
#include <snk/gui/gui_theme_desc.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/resource/resources/res_resource_text.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }
namespace desc { class desc_system; }
namespace gui { class theme_service; }

namespace edt
{
	// The person's choice of theme (#1109): edt://settings/editor_theme.json, {"theme": "<preset tag>"}.
	// Never the project.
	inline constexpr std::string_view k_theme_setting = "editor_theme.json";
	// Where the person's own presets and fonts are; edt://themes/** is in the default edt:// write patterns.
	inline constexpr std::string_view k_personal_themes = "edt://themes/";
	inline constexpr std::string_view k_personal_fonts = "edt://fonts/";

	// nullopt for a text that is no JSON object with a string "theme".
	std::optional<res::tag> chosen_theme_of(std::string_view json_text);
	// `json_text` (any text; a broken one starts over) with "theme" set, other keys kept.
	std::string with_chosen_theme(std::string_view json_text, const res::tag& theme);
	// 1..64 of [A-Za-z0-9_-]: the file name of a personal preset, without ".desc".
	bool valid_theme_name(std::string_view name);
	res::tag personal_theme_tag(std::string_view name);
	bool is_personal_theme(const res::tag& tag);
	// "dark_neutral" of engine://themes/dark_neutral.desc.
	std::string theme_name_of(const res::tag& tag);

	// What the Editor Settings window shows and does, without ImGui. The window is the only writer: no command
	// reaches it.
	class theme_settings_model
	{
	public:
		struct ports
		{
			res::resource_system& resources;
			desc::desc_system& descs;
			// Null without a renderer (headless): nothing is applied or written then.
			gui::theme_service* themes = nullptr;
			// Whether gui/theme names a preset (engine::gui::theme_set_by_config): the person's choice is not applied.
			bool theme_set_by_config = false;
			// The files below a folder of a protocol; empty: resource_system::files_under (disk mounts only).
			std::function<std::vector<res::tag>(std::string_view protocol, std::string_view folder)> list_files;
		};

		explicit theme_settings_model(ports from);

		// Lets the service read edt://themes/ and edt://fonts/, and starts reading the person's choice.
		void start();
		// Advances the reads; once a frame.
		void poll();

		bool available() const { return m_ports.themes != nullptr; }
		// The shipped presets, then the person's, each sorted by name.
		const std::vector<res::tag>& presets() const { return m_presets; }
		// The UI font, then the TTF/OTF files of edt://fonts/.
		const std::vector<res::tag>& fonts() const { return m_fonts; }
		void refresh_lists();
		res::tag current() const;

		// Applies `preset`, keeps it as the person's choice and loads it for editing.
		void choose(const res::tag& preset);

		// The preset loaded for editing (a copy: edits change neither the file nor the cache until save); null while it
		// loads or when it could not be loaded.
		gui::theme_desc* working() { return m_working.get(); }
		const res::tag& working_tag() const { return m_working_tag; }
		bool editable() const { return is_personal_theme(m_working_tag); }
		bool dirty() const { return m_dirty; }
		// After an edit of working(): shows it at once, without writing.
		void edited();
		// Writes working() to its edt://themes/ file and applies it from there. The error text, or nullopt.
		std::optional<std::string> save();
		// Drops the edits: the file's version is loaded and applied again.
		void revert();
		// Writes working() as edt://themes/<name>.desc, which must not exist yet, and chooses it.
		std::optional<std::string> duplicate(std::string_view name);
		const std::string& last_message() const { return m_message; }

	private:
		void load_working(const res::tag& preset);
		std::optional<std::string> write_preset(const res::tag& tag, const gui::theme_desc& preset);
		void remember_choice(const res::tag& preset);

		ports m_ports;
		std::vector<res::tag> m_presets;
		std::vector<res::tag> m_fonts;
		res::res_handle<res::text_resource> m_choice;
		res::res_handle<gui::theme_desc> m_loading;
		res::tag m_working_tag;
		std::unique_ptr<gui::theme_desc> m_working;
		bool m_dirty = false;
		std::string m_message;
	};
}
