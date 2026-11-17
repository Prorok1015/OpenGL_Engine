#include "edt_prefab_variant_creator.h"
#include <snk/desc/desc_pretty_json.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <format>

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
	for (int number = 1; number < 1000; ++number) {
		std::string candidate = number == 1 ? std::format("{}_variant", stem) : std::format("{}_variant{}", stem, number);
		if (check(base, candidate).empty()) return candidate;
	}
	return {};
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
	if (const std::optional<std::string> why = m_ports.blocker ? m_ports.blocker() : std::nullopt) return { {}, *why };
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
		{ "__type", prefab_type }, { "__version", descs->current_version(prefab_type) }, { "__parent", std::string{ base.view() } } };
	const res::tag tag = res::tag::make(relative);
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
