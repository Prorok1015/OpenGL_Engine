#include "edt_model_import_form.h"
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include <utility>

bool edt::model_import_settings_form::open(std::filesystem::path source, import_fn on_import)
{
	if (is_open() || !on_import) return false;
	m_shown_name = text_escape::escaped_text(fs_paths::utf8_of(source.filename()), 256);
	m_source = std::move(source);
	m_settings = {};
	m_target.clear();
	m_notes.clear();
	m_on_import = std::move(on_import);
	return true;
}

bool edt::model_import_settings_form::open_reimport(std::filesystem::path source, std::string target,
	const model_import_settings& settings, std::vector<std::string> notes, import_fn on_import)
{
	if (!open(std::move(source), std::move(on_import))) return false;
	m_target = std::move(target);
	m_settings = settings;
	m_notes = std::move(notes);
	return true;
}

void edt::model_import_settings_form::submit()
{
	if (!is_open()) return;
	// Closed before the call: the import may open the window again for another model.
	import_fn on_import = std::exchange(m_on_import, nullptr);
	const std::filesystem::path source = m_source;
	const model_import_settings settings = m_settings;
	close();
	on_import(source, settings);
}

void edt::model_import_settings_form::cancel()
{
	m_on_import = nullptr;
	close();
}

void edt::model_import_settings_form::close()
{
	m_source.clear();
	m_shown_name.clear();
	m_settings = {};
	m_target.clear();
	m_notes.clear();
}
