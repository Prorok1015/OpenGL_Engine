#include "edt_material_creator.h"
#include <snk/resource/res_free_name.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	// The browser's kind lookup lowercases the path, so a suffix written in any case is the kind's.
	bool ends_with_ignoring_case(std::string_view text, std::string_view suffix)
	{
		if (text.size() < suffix.size()) {
			return false;
		}
		const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
		const std::string_view tail = text.substr(text.size() - suffix.size());
		return std::equal(tail.begin(), tail.end(), suffix.begin(), [&lower](char a, char b) { return lower(a) == lower(b); });
	}
}

json::object edt::material_fields(const material_options& options)
{
	const std::string_view parent = options.parent.view().empty() ? k_base_material : options.parent.view();
	json::object out{ { "__parent", std::string(parent) } };
	if (!options.shader.view().empty()) {
		out["shader"] = std::string(options.shader.view());
	}
	return out;
}

void edt::add_material_defaults(json::object& desc)
{
	const json::value* type = desc.if_contains("__type");
	if (!type || !type->is_string() || std::string_view(type->as_string()) != k_material_type || desc.contains("__parent")) {
		return;
	}
	desc["__parent"] = material_fields().at("__parent");
}

edt::material_menu_item edt::material_menu_item_of(std::string_view type)
{
	if (type == k_shader_type) {
		return material_menu_item::from_shader;
	}
	if (type == k_material_type) {
		return material_menu_item::variant;
	}
	return material_menu_item::none;
}

std::optional<edt::material_source> edt::material_creator::source_of(const res::tag& file, std::string_view type) const
{
	constexpr std::string_view protocol = "res://";
	constexpr std::string_view plain = ".desc";
	const std::string_view view = file.view();
	if (!view.starts_with(protocol) || !ends_with_ignoring_case(view, plain)) {
		return std::nullopt;
	}
	const std::string_view relative = view.substr(protocol.size());
	const std::size_t slash = relative.rfind('/');
	const std::string_view name = slash == std::string_view::npos ? relative : relative.substr(slash + 1);
	const std::string own = m_names.suffix_of(type);
	const std::size_t cut = !own.empty() && ends_with_ignoring_case(name, own) ? own.size() : plain.size();
	if (name.size() <= cut) {
		return std::nullopt;
	}
	return material_source{ .folder = slash == std::string_view::npos ? std::string{} : std::string(relative.substr(0, slash)),
		.stem = std::string(name.substr(0, name.size() - cut)),
		.file = std::string(name) };
}

std::string edt::material_creator::suggested_name(std::string_view folder) const
{
	return suggested_name(folder, k_material_name, "-");
}

std::string edt::material_creator::suggested_name(std::string_view folder, std::string_view base,
	std::string_view separator) const
{
	if (base.empty() || !kind_problem().empty()) {
		return {};
	}
	const std::string own_suffix = suffix();
	// A candidate the editor's own name rules refuse is passed over for the next number, as the layers' taken names are.
	const auto refused = [this, folder](std::string_view relative) {
		const std::size_t slash = relative.rfind('/');
		return !check(folder, slash == std::string_view::npos ? relative : relative.substr(slash + 1)).empty();
	};
	const res::free_name_result found = res::free_name(m_names.layers(), folder, base, own_suffix, separator, refused);
	if (!found.ok() || found.name.size() <= own_suffix.size()) {
		return {};
	}
	return found.name.substr(0, found.name.size() - own_suffix.size());
}

std::string edt::material_creator::kind_problem() const
{
	if (suffix() == k_material_suffix) {
		return {};
	}
	return std::format("no asset kind names {} files '{}': materials cannot be created", k_material_type, k_material_suffix);
}

std::string edt::material_creator::check(std::string_view folder, std::string_view name) const
{
	if (std::string problem = kind_problem(); !problem.empty()) {
		return problem;
	}
	return m_names.check(folder, k_material_type, name);
}

edt::document_creation edt::material_creator::create(std::string_view folder, std::string_view name,
	const material_options& options) const
{
	if (std::string problem = kind_problem(); !problem.empty()) {
		return document_creation{ .tag = {}, .error = std::move(problem) };
	}
	return m_names.create(folder, k_material_type, name, material_fields(options));
}

void edt::material_create_form::open(std::string folder, material_options options)
{
	std::string suggestion = m_creator.suggested_name(folder);
	open(std::move(folder), std::move(options), std::move(suggestion), std::string(k_material_title));
}

void edt::material_create_form::open(std::string folder, material_options options, std::string suggestion, std::string title)
{
	m_title = std::move(title);
	m_folder = std::move(folder);
	m_options = std::move(options);
	m_suggested = std::move(suggestion);
	m_name = m_suggested;
	recheck();
}

bool edt::material_create_form::open_from_source(const res::tag& file, std::string_view type, material_options options,
	std::string_view tail, std::string_view separator, std::string_view title_head)
{
	const std::optional<material_source> source = m_creator.source_of(file, type);
	if (!source) {
		return false;
	}
	std::string suggestion = m_creator.suggested_name(source->folder, source->stem + std::string(tail), separator);
	open(source->folder, std::move(options), std::move(suggestion), std::format("{} {}", title_head, source->file));
	return true;
}

bool edt::material_create_form::open_from_shader(const res::tag& shader)
{
	material_options options;
	options.shader = shader;
	return open_from_source(shader, k_shader_type, std::move(options), "", "-", std::format("{} from", k_material_title));
}

bool edt::material_create_form::open_variant_of(const res::tag& material)
{
	material_options options;
	options.parent = material;
	// As prefab_variant_creator::suggested_name: "<stem>_variant", then "<stem>_variant2", no separator before the number.
	return open_from_source(material, k_material_type, std::move(options), "_variant", "", "Material Variant of");
}

void edt::material_create_form::recheck()
{
	m_file = m_name.empty() ? std::string{} : m_creator.file_name(m_name);
	m_problem = m_name.empty() ? m_creator.kind_problem() : m_creator.check(m_folder, m_name);
	m_error.clear();
}

void edt::material_create_form::set_name(std::string name)
{
	if (name == m_name) {
		return;
	}
	m_name = std::move(name);
	recheck();
}

std::optional<res::tag> edt::material_create_form::submit()
{
	if (!can_submit()) {
		return std::nullopt;
	}
	if (const std::string shown = m_file; m_creator.file_name(m_name) != shown) {
		recheck();
		m_error = std::format(
			"the file would now be '{}', not '{}' (the asset kinds changed): check it and create again", m_file, shown);
		return std::nullopt;
	}
	const document_creation made = m_creator.create(m_folder, m_name, m_options);
	if (!made.ok()) {
		m_error = made.error;
		return std::nullopt;
	}
	m_error.clear();
	return made.tag;
}
