#include "edt_prefab_variant_creator.h"
#include <snk/desc/desc_pretty_json.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_free_name.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <vector>

namespace json = boost::json;

bool edt::prefab_variant_creator::split(const res::tag& base, std::string& folder, std::string& stem)
{
	constexpr std::string_view protocol = "res://";
	const std::string_view view = base.view();
	if (!view.starts_with(protocol) || !view.ends_with(file_suffix)) return false;
	const std::string_view relative = view.substr(protocol.size(), view.size() - protocol.size() - file_suffix.size());
	if (relative.empty()) return false;
	const std::size_t slash = relative.rfind('/');
	folder = slash == std::string_view::npos ? std::string{} : std::string{ relative.substr(0, slash) };
	stem = std::string{ slash == std::string_view::npos ? relative : relative.substr(slash + 1) };
	return !stem.empty();
}

std::string edt::prefab_variant_creator::suggested_name(const res::tag& base) const
{
	std::string folder, stem;
	if (!split(base, folder, stem)) return {};
	const std::vector<std::filesystem::path> layers = m_ports.layers ? m_ports.layers() : std::vector<std::filesystem::path>{};
	// "hero_variant", "hero_variant2", ...: no separator before the number. A candidate the editor's own name rules refuse is
	// passed over for the next number, as the layers' taken names are.
	const auto refused = [this, &base](std::string_view relative) {
		const std::size_t slash = relative.rfind('/');
		std::string_view name = slash == std::string_view::npos ? relative : relative.substr(slash + 1);
		name.remove_suffix(file_suffix.size());
		return !check(base, name).empty();
	};
	const res::free_name_result found = res::free_name(layers, folder, std::format("{}_variant", stem), file_suffix, "", refused);
	if (!found.ok()) return {};
	return found.name.substr(0, found.name.size() - file_suffix.size());
}

std::string edt::prefab_variant_creator::check(const res::tag& base, std::string_view stem) const
{
	std::string folder, base_stem;
	if (!split(base, folder, base_stem)) return std::format("'{}' is not a prefab file of the project (res://...{})", base.view(), file_suffix);
	if (stem.empty()) return "give the variant a name";
	return m_names.check(folder, std::format("{}.prefab", stem));
}

edt::document_creation edt::prefab_variant_creator::create(const res::tag& base, std::string_view stem) const
{
	if (const std::optional<std::string> why = m_ports.blocker ? m_ports.blocker() : std::nullopt) return { {}, *why, true };
	const desc::desc_system* descs = m_ports.descs ? m_ports.descs() : nullptr;
	res::resource_system* resources = m_ports.resources ? m_ports.resources() : nullptr;
	if (!descs || !resources) return { {}, "the editor has no project open" };

	std::string folder, base_stem;
	if (!split(base, folder, base_stem)) return { {}, std::format("'{}' is not a prefab file of the project (res://...{})", base.view(), file_suffix) };
	const std::string name = stem.empty() ? suggested_name(base) : std::string{ stem };
	if (std::string problem = check(base, name); !problem.empty()) return { {}, std::move(problem) };

	const std::string relative = folder.empty() ? std::format("{}{}", name, file_suffix) : std::format("{}/{}{}", folder, name, file_suffix);
	const std::string prefab_type{ "prefab_desc" };
	const json::value content = json::object{
		{ "__type", prefab_type }, { "__version", descs->current_version(prefab_type) }, { "__parent", std::string{ base.view() } }, { "__layer", 2 } };
	const res::tag tag = res::tag::make(relative);
	const auto base_prefab = resources->require_sync<scn::prefab_desc>(base);
	if (!base_prefab.is_ready() || base_prefab.has_error()) return { {}, std::format("'{}' does not load as a prefab", base.view()) };
	if (m_claim) {
		if (std::string claimed = m_claim(tag); !claimed.empty()) return { {}, std::move(claimed), true };
	}
	if (const res::file_operation made = resources->create_sync(tag, desc::pretty_json_bytes(content)); !made.ok) return { {}, made.error };
	return { tag, {} };
}

void edt::prefab_variant_form::open(res::tag base)
{
	m_base = std::move(base);
	m_name = m_creator.suggested_name(m_base);
	m_problem = m_name.empty() ? m_creator.check(m_base, m_name) : std::string{};
	m_error.clear();
}

void edt::prefab_variant_form::set_name(std::string name)
{
	if (name == m_name) return;
	m_name = std::move(name);
	m_problem = m_name.empty() ? std::string{} : m_creator.check(m_base, m_name);
	m_error.clear();
}

std::optional<res::tag> edt::prefab_variant_form::submit()
{
	if (!can_submit()) return std::nullopt;
	const document_creation made = m_creator.create(m_base, m_name);
	if (!made.ok()) {
		m_error = made.error;
		return std::nullopt;
	}
	m_error.clear();
	return made.tag;
}

std::string edt::open_level_claim(res::resource_system& resources, const desc::desc_system& descs, const desc_file_layout& layout, const res::tag& tag, bool agent)
{
	const desc_file_read probe = read_desc_file(resources, descs, layout, tag);
	if (probe.in_open_level || (agent && probe.level_unreadable)) return probe.level_refusal;
	return {};
}
