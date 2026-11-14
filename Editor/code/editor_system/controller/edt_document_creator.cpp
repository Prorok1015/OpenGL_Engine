#include "edt_document_creator.h"
#include "edt_asset_picker.h"
#include "edt_file_menu.h"
#include <snk/common/fs_paths.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cctype>
#include <boost/json.hpp>
#include <format>
#include <system_error>

namespace json = boost::json;

std::vector<std::string> edt::document_creator::types() const
{
	const desc::desc_system* descs = m_ports.descs ? m_ports.descs() : nullptr;
	return descs ? descs->document_types() : std::vector<std::string>{};
}

std::string edt::document_creator::file_name(std::string_view name)
{
	constexpr std::string_view extension = ".desc";
	std::string lower{ name };
	std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower.ends_with(extension) ? std::string{ name } : std::string{ name } + std::string{ extension };
}

std::string edt::document_creator::check(std::string_view folder, std::string_view name) const
{
	if (name.empty()) {
		return "give the document a name";
	}
	const std::string file = file_name(name);
	if (const agent_level_path checked = check_agent_name(file); !checked.ok()) {
		return checked.error;
	}
	if (res::is_skipped_asset_name(file, false)) {
		return "the asset list hides a name like this (a leading dot, a temporary file)";
	}
	if (!folder.empty()) {
		if (const agent_level_path checked = check_agent_folder(folder); !checked.ok()) {
			return checked.error;
		}
	}
	const std::string relative = folder.empty() ? file : std::format("{}/{}", folder, file);
	if (const agent_level_path checked = check_agent_level_path(relative); !checked.ok()) {
		return checked.error;
	}
	const std::vector<std::filesystem::path> layers = m_ports.layers ? m_ports.layers() : std::vector<std::filesystem::path>{};
	const std::string key = res::asset_name_key(file, res::asset_names_case_insensitive);
	for (std::size_t i = layers.size(); i-- > 0;) {
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(layers[i] / fs_paths::path_from_utf8(folder), ec)) {
			if (res::asset_name_key(fs_paths::utf8_of(entry.path().filename()), res::asset_names_case_insensitive) == key) {
				return std::format("'{}' already exists in the {} layer", file, picker::layer_label(i, layers.size()));
			}
		}
	}
	return {};
}

edt::document_creation edt::document_creator::create(std::string_view folder, std::string_view type, std::string_view name) const
{
	if (std::string problem = check(folder, name); !problem.empty()) {
		return { {}, std::move(problem) };
	}
	const std::vector<std::string> known = types();
	if (std::ranges::find(known, type) == known.end()) {
		return { {}, std::format("'{}' is not a document type", type) };
	}
	const desc::desc_system* descs = m_ports.descs ? m_ports.descs() : nullptr;
	res::resource_system* resources = m_ports.resources ? m_ports.resources() : nullptr;
	if (!descs || !resources) {
		return { {}, "the editor has no project open" };
	}
	const std::string file = file_name(name);
	const std::string relative = folder.empty() ? file : std::format("{}/{}", folder, file);
	const json::value content = json::object{ { "__type", std::string(type) }, { "__version", descs->current_version(type) } };
	const res::tag tag = res::tag::make(relative);
	if (const res::file_operation made = resources->create_sync(tag, desc::pretty_json_bytes(content)); !made.ok) {
		return { {}, made.error };
	}
	return { tag, {} };
}

void edt::document_create_form::open(std::string folder)
{
	m_open = true;
	m_folder = std::move(folder);
	m_types = m_creator.types();
	m_type_index = 0;
	m_name.clear();
	m_error.clear();
}

void edt::document_create_form::set_name(std::string name)
{
	m_name = std::move(name);
	m_error.clear();
}

std::string edt::document_create_form::problem() const
{
	return m_name.empty() ? std::string{} : m_creator.check(m_folder, m_name);
}

std::optional<res::tag> edt::document_create_form::submit()
{
	if (!can_submit()) {
		return std::nullopt;
	}
	const document_creation made = m_creator.create(m_folder, m_types[m_type_index], m_name);
	if (!made.ok()) {
		m_error = made.error;
		return std::nullopt;
	}
	m_open = false;
	m_error.clear();
	return made.tag;
}
