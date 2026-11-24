#include "edt_document_creator.h"
#include "edt_asset_picker.h"
#include "edt_file_menu.h"
#include <snk/common/fs_paths.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_free_name.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cctype>
#include <boost/json.hpp>
#include <format>

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
	if (lower.ends_with(extension)) {
		return std::string{ name.substr(0, name.size() - extension.size()) } + std::string{ extension };
	}
	return std::string{ name } + std::string{ extension };
}

std::string edt::document_creator::check(std::string_view folder, std::string_view name) const
{
	if (name.empty()) {
		return "give the document a name";
	}
	const std::string file = file_name(name);
	if (file.size() <= std::string_view{ ".desc" }.size()) {
		return "give the document a name";
	}
	if (name.back() == '.' || name.back() == ' ') {
		return "a name does not end with '.' or ' '";
	}
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
	const std::optional<res::name_refusal> taken = res::name_taken(layers, relative);
	if (!taken) {
		return {};
	}
	const std::string layer_name = picker::layer_label(taken->layer, layers.size());
	if (!taken->error.empty()) {
		return std::format("the folder cannot be read in the {} layer: {}", layer_name, taken->error);
	}
	if (taken->meta_left) {
		return std::format("a '{}{}' of a deleted file is left in the {} layer: remove it first", file, res::asset_meta_suffix, layer_name);
	}
	const std::string holder = fs_paths::utf8_of(taken->existing.filename());
	if (res::asset_name_key(holder, true) == res::asset_name_key(file, true)) {
		return std::format("'{}' already exists in the {} layer", file, layer_name);
	}
	return std::format("a '{}' is left in the {} layer: remove it first", holder, layer_name);
}

edt::document_creation edt::document_creator::create(std::string_view folder, std::string_view type, std::string_view name) const
{
	if (const std::optional<std::string> why = m_ports.blocker ? m_ports.blocker() : std::nullopt) {
		return { {}, *why };
	}
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
	m_folder = std::move(folder);
	m_types = m_creator.types();
	m_type_index = 0;
	m_name.clear();
	m_problem.clear();
	m_error.clear();
}

void edt::document_create_form::set_name(std::string name)
{
	if (name == m_name) {
		return;
	}
	m_name = std::move(name);
	m_problem = m_name.empty() ? std::string{} : m_creator.check(m_folder, m_name);
	m_error.clear();
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
	m_error.clear();
	return made.tag;
}
