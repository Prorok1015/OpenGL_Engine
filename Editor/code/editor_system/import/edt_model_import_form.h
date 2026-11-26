#pragma once
#include "edt_model_import_settings.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// What the "Model Import Settings" window holds between frames (#1195 PR 2), without ImGui: the picked model, the
// settings the person chose and what Import does with them. Cancel imports nothing.
namespace edt {

	class model_import_settings_form
	{
	public:
		using import_fn = std::function<void(const std::filesystem::path&, const model_import_settings&)>;

		// Shows `source` with the default settings; Import calls `on_import` once. False (nothing changes) while the
		// window is open with another model.
		bool open(std::filesystem::path source, import_fn on_import);
		// Re-import mode (#1194): over `target` (the prefab's res:// tag), starting from the `settings` its `.resource`
		// records; `notes` are shown and block nothing (another file name, the same file as last time). False as open().
		bool open_reimport(std::filesystem::path source, std::string target, const model_import_settings& settings,
			std::vector<std::string> notes, import_fn on_import);
		bool reimport() const { return !m_target.empty(); }
		const std::string& target() const { return m_target; }
		const std::vector<std::string>& notes() const { return m_notes; }
		bool is_open() const { return static_cast<bool>(m_on_import); }

		const std::filesystem::path& source() const { return m_source; }
		// The file name, escaped: it is someone else's text.
		const std::string& shown_name() const { return m_shown_name; }

		const model_import_settings& settings() const { return m_settings; }
		void set_normals(model_normals normals) { m_settings.normals = normals; }
		void set_tangents(bool tangents) { m_settings.tangents = tangents; }

		// Closes the window and imports with the settings shown.
		void submit();
		// Closes the window; nothing is imported.
		void cancel();

	private:
		void close();

		std::filesystem::path m_source;
		std::string m_shown_name;
		std::string m_target;
		std::vector<std::string> m_notes;
		model_import_settings m_settings;
		import_fn m_on_import;
	};

} // namespace edt
