#include "edt_material_creator.h"
#include <snk/resource/res_free_name.h>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

namespace json = boost::json;

json::object edt::material_fields(const material_options& options)
{
	const std::string_view parent = options.parent.view().empty() ? k_base_material : options.parent.view();
	json::object out{ { "__parent", std::string(parent) } };
	if (!options.shader.view().empty()) {
		out["shader"] = std::string(options.shader.view());
	}
	return out;
}

edt::material_menu_item edt::material_menu_item_of(std::string_view kind)
{
	if (kind == k_shader_type) {
		return material_menu_item::from_shader;
	}
	if (kind == k_material_type) {
		return material_menu_item::variant;
	}
	return material_menu_item::none;
}

std::optional<edt::material_source> edt::material_creator::source_of(const res::tag& file, std::string_view type) const
{
	constexpr std::string_view protocol = "res://";
	constexpr std::string_view plain = ".desc";
	const std::string_view view = file.view();
	if (!view.starts_with(protocol) || !view.ends_with(plain)) {
		return std::nullopt;
	}
	const std::string_view relative = view.substr(protocol.size());
	const std::size_t slash = relative.rfind('/');
	const std::string_view name = slash == std::string_view::npos ? relative : relative.substr(slash + 1);
	const std::string own = m_names.suffix_of(type);
	const std::size_t cut = name.ends_with(own) ? own.size() : plain.size();
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

bool edt::material_create_form::open_from_shader(const res::tag& shader)
{
	const std::optional<material_source> source = m_creator.source_of(shader, k_shader_type);
	if (!source) {
		return false;
	}
	material_options options;
	options.shader = shader;
	std::string suggestion = m_creator.suggested_name(source->folder, source->stem, "-");
	open(source->folder, std::move(options), std::move(suggestion), std::format("{} from {}", k_material_title, source->file));
	return true;
}

bool edt::material_create_form::open_variant_of(const res::tag& material)
{
	const std::optional<material_source> source = m_creator.source_of(material, k_material_type);
	if (!source) {
		return false;
	}
	material_options options;
	options.parent = material;
	// As prefab_variant_creator::suggested_name: "<stem>_variant", then "<stem>_variant2", no separator before the number.
	std::string suggestion = m_creator.suggested_name(source->folder, std::format("{}_variant", source->stem), "");
	open(source->folder, std::move(options), std::move(suggestion), std::format("Material Variant of {}", source->file));
	return true;
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
