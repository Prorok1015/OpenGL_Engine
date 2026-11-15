#include "edt_project_trust.h"
#include "edt_settings.h"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <format>
#include <string>
#include <system_error>
#include <utility>

namespace json = boost::json;

namespace {

	constexpr std::string_view k_projects = "projects";
	constexpr std::string_view k_trust = "trust";
	constexpr std::string_view k_own = "own";

	json::object read_object(const std::optional<std::string>& text)
	{
		if (!text) return {};
		std::error_code ec;
		json::value doc = json::parse(*text, ec);
		if (ec || !doc.is_object()) return {};
		return std::move(doc.get_object());
	}

	json::object& project_entry(json::object& root, std::string_view project)
	{
		json::value& projects = root[k_projects];
		if (!projects.is_object()) projects = json::object{};
		json::value& entry = projects.get_object()[project];
		if (!entry.is_object()) entry = json::object{};
		return entry.get_object();
	}

	std::optional<std::string> stored(res::resource_system& res, const std::string& text)
	{
		if (!edt::write_setting(res, edt::k_project_trust_settings, text)) {
			return std::format("edt://settings/{} cannot be written", edt::k_project_trust_settings);
		}
		return std::nullopt;
	}

}

edt::project_trust_record edt::project_trust_of(const std::optional<std::string>& settings_text,
	std::string_view project)
{
	project_trust_record record;
	const json::object root = read_object(settings_text);
	const json::value* projects = root.if_contains(k_projects);
	if (!projects || !projects->is_object()) return record;
	const json::value* entry = projects->get_object().if_contains(project);
	if (!entry || !entry->is_object()) return record;
	if (const json::value* trust = entry->get_object().if_contains(k_trust); trust && trust->is_bool()) {
		record.answer = trust->get_bool() ? trust_answer::trusted : trust_answer::refused;
	}
	if (const json::value* own = entry->get_object().if_contains(k_own); own && own->is_object()) {
		for (const auto& [module, hash] : own->get_object()) {
			if (hash.is_string()) record.own_builds.emplace(std::string(module), std::string(hash.get_string()));
		}
	}
	return record;
}

std::string edt::with_trust_answer(const std::optional<std::string>& settings_text, std::string_view project,
	trust_answer answer)
{
	json::object root = read_object(settings_text);
	json::object& entry = project_entry(root, project);
	if (answer == trust_answer::none) {
		entry.erase(k_trust);
	} else {
		entry[k_trust] = answer == trust_answer::trusted;
	}
	return json::serialize(root);
}

std::string edt::with_own_build(const std::optional<std::string>& settings_text, std::string_view project,
	std::string_view module, std::string_view sha256)
{
	json::object root = read_object(settings_text);
	json::object& entry = project_entry(root, project);
	json::value& own = entry[k_own];
	if (!own.is_object()) own = json::object{};
	own.get_object()[module] = sha256;
	return json::serialize(root);
}

edt::library_verdict edt::judge_library(const project_trust_record& record, std::string_view module,
	const std::optional<std::string>& sha256)
{
	if (record.answer == trust_answer::trusted) return library_verdict::load;
	if (record.answer == trust_answer::refused) return library_verdict::refuse;
	const auto own = record.own_builds.find(std::string(module));
	if (sha256 && own != record.own_builds.end() && own->second == *sha256) return library_verdict::load;
	return library_verdict::ask;
}

edt::project_trust_record edt::read_project_trust(res::resource_system& res, std::string_view project)
{
	return project_trust_of(read_setting(res, k_project_trust_settings), project);
}

std::optional<std::string> edt::store_trust_answer(res::resource_system& res, std::string_view project,
	trust_answer answer)
{
	return stored(res, with_trust_answer(read_setting(res, k_project_trust_settings), project, answer));
}

std::optional<std::string> edt::store_own_build(res::resource_system& res, std::string_view project,
	std::string_view module, std::string_view sha256)
{
	return stored(res, with_own_build(read_setting(res, k_project_trust_settings), project, module, sha256));
}
