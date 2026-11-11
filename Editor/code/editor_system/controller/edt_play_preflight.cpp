#include "edt_play_preflight.h"
#include "edt_scene_document.h"
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <string>

namespace json = boost::json;

namespace {

	const char* plan_kind_name(ecs::diagnostic_kind kind)
	{
		switch (kind) {
		case ecs::diagnostic_kind::unknown_system: return "unknown_system";
		case ecs::diagnostic_kind::duplicate_system: return "duplicate_system";
		case ecs::diagnostic_kind::unknown_phase: return "unknown_phase";
		case ecs::diagnostic_kind::unknown_constraint_target: return "unknown_constraint_target";
		case ecs::diagnostic_kind::wrong_phase_constraint: return "wrong_phase_constraint";
		case ecs::diagnostic_kind::cycle: return "cycle";
		case ecs::diagnostic_kind::access_conflict: return "access_conflict";
		}
		return "";
	}

	// RFC 6901: '~' and '/' in a key.
	std::string pointer_token(std::string_view key)
	{
		std::string out;
		out.reserve(key.size());
		for (const char c : key) {
			if (c == '~') out += "~0";
			else if (c == '/') out += "~1";
			else out += c;
		}
		return out;
	}

	bool same(const edt::problem& a, const edt::problem& b)
	{
		return a.severity == b.severity && a.file == b.file && a.code == b.code && a.message == b.message && a.notes == b.notes;
	}

	struct walker
	{
		const edt::scene_document& scene;
		const edt::play_preflight_services& services;
		std::vector<edt::preflight_problem>& out;
		std::string file;
		std::string level;
		std::string world;

		void add(edt::problem_severity severity, std::string code, std::string message, const std::vector<std::uint64_t>& path,
			const std::string& json_path)
		{
			edt::preflight_problem found;
			found.item.severity = severity;
			found.item.file = file;
			found.item.code = std::move(code);
			found.item.message = std::format("world '{}': {}", world, message);
			if (!json_path.empty()) found.item.notes.push_back(json_path);
			found.target.level = level;
			found.target.world = world;
			found.target.path = path;
			out.push_back(std::move(found));
		}

		// A string is taken for a reference when it names a scheme ("scheme://..."): a plain name is not one.
		void check_reference(const std::string& text, const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			if (text.find("://") == std::string::npos) return;
			const res::tag reference{ text };
			if (std::string why = reference.protocol().empty() ? res::no_protocol_reason(reference)
				: res::reference_problem(reference, scene.level_tag.protocol()); !why.empty()) {
				add(edt::problem_severity::error, "reference", why, path, json_path);
				return;
			}
			if (services.resources && !services.resources->exists(reference)) {
				add(edt::problem_severity::error, "missing_resource", std::format("'{}' does not exist", text), path, json_path);
			}
		}

		void check_values(const json::value& value, const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			if (const auto* text = value.if_string()) {
				check_reference(std::string(*text), path, json_path);
			} else if (const auto* object = value.if_object()) {
				for (const auto& member : *object) {
					check_values(member.value(), path, json_path + "/" + pointer_token(member.key()));
				}
			} else if (const auto* array = value.if_array()) {
				for (std::size_t i = 0; i < array->size(); ++i) check_values((*array)[i], path, std::format("{}/{}", json_path, i));
			}
		}

		void check_component(const std::string& key, const scn::prefab_desc::prefab_comp_node& component,
			const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			const std::string& type = component.type_name.empty() ? key : component.type_name;
			if (component.parent_desc.is_valid() && component.parent_desc.has_error()) {
				add(edt::problem_severity::error, "missing_resource", std::format("component '{}': its __parent '{}' did not load", key,
					component.parent_desc.get_tag().view()), path, json_path + "/__parent");
			}
			if (services.descs && !services.descs->is_registered(type)) {
				add(edt::problem_severity::error, "unknown_component", std::format("component '{}': the type '{}' is not registered", key, type),
					path, json_path);
				return;
			}
			if (services.descs) {
				if (const desc::type_schema* schema = services.descs->find_schema(type)) {
					// What the desc system keeps beside the fields (__version, __type) is no field of the schema.
					json::object fields;
					for (const auto& member : component.overrides) {
						if (!member.key().starts_with("__")) fields.emplace(member.key(), member.value());
					}
					const desc::override_failure bad = desc::check_override_object(*schema, fields, nullptr, &services.descs->schemas());
					if (bad.status != desc::override_status::valid) {
						add(edt::problem_severity::error, "schema", std::format("component '{}' ({}): {} ({})", key, type,
							std::string_view(bad.pointer).substr(bad.pointer.empty() ? 0 : 1),
							bad.status == desc::override_status::stale ? "no such field" : "the value does not fit"), path, json_path + bad.pointer);
					}
				}
			}
			if (services.assembler && !services.assembler->has_spawner(type)) {
				add(edt::problem_severity::warning, "no_spawner", std::format("component '{}': nothing spawns the type '{}', it makes no component",
					key, type), path, json_path);
			}
			check_values(json::value(component.overrides), path, json_path);
			if (services.check_component) {
				services.check_component(edt::preflight_component{ world, path, key, component, json_path }, out);
			}
		}

		void check_node(const scn::prefab_desc::prefab_node& node, std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			// Sorted: the same document gives the same rows in the same order.
			std::vector<const std::string*> keys;
			keys.reserve(node.components.size());
			for (const auto& [key, component] : node.components) keys.push_back(&key);
			std::ranges::sort(keys, [](const std::string* a, const std::string* b) { return *a < *b; });
			for (const std::string* key : keys) {
				check_component(*key, node.components.at(*key), path, json_path + "/components/" + pointer_token(*key));
			}
			for (std::size_t i = 0; i < node.children.size(); ++i) {
				path.push_back(node.children[i].node_id);
				check_node(node.children[i], path, std::format("{}/children/{}", json_path, i));
				path.pop_back();
			}
		}
	};
}

std::vector<edt::preflight_problem> edt::run_play_preflight(const scene_document& scene, const play_preflight_services& services)
{
	std::vector<preflight_problem> out;
	const std::size_t count = std::min(scene.world_names.size(), scene.world_descs.size());
	walker walk{ scene, services, out, std::string(scene.level_tag.relative()), scene.level_tag.string(), {} };
	for (std::size_t index = 0; index < count; ++index) {
		walk.world = scene.world_names[index];
		const std::vector<std::uint64_t> world_path;
		if (services.systems) {
			const std::vector<std::string>& listed = index < scene.worlds_systems_list.size() ? scene.worlds_systems_list[index]
				: scene.world_descs[index].get_systems();
			// The log lines of a plan are the world's own when it is built; this one is only looked at.
			ecs::plan_options options;
			options.validate_access = false;
			const ecs::system_plan plan = ecs::build_system_plan(*services.systems, listed, options);
			for (const ecs::plan_diagnostic& diagnostic : plan.diagnostics) {
				walk.add(diagnostic.is_error() ? problem_severity::error : problem_severity::warning, plan_kind_name(diagnostic.kind),
					diagnostic.message, world_path, std::format("/worlds/{}/systems", index));
			}
		}
		std::vector<std::uint64_t> path;
		walk.check_node(scene.world_descs[index].get_root(), path, std::format("/worlds/{}", index));
	}
	return out;
}

bool edt::has_errors(const std::vector<preflight_problem>& problems)
{
	return std::ranges::any_of(problems, [](const preflight_problem& p) { return p.item.severity == problem_severity::error; });
}

void edt::play_preflight_publisher::publish(problem_list& problems, std::vector<preflight_problem> found)
{
	const bool unchanged = found.size() == m_last.size()
		&& std::equal(found.begin(), found.end(), m_last.begin(), [](const preflight_problem& a, const preflight_problem& b) { return same(a.item, b.item); });
	const bool was_published = !m_last.empty();
	m_last = std::move(found);
	if (unchanged && was_published) return;
	if (m_last.empty()) {
		if (was_published) problems.clear(source);
		return;
	}
	std::vector<problem> rows;
	rows.reserve(m_last.size());
	for (const preflight_problem& p : m_last) rows.push_back(p.item);
	problems.set(source, std::move(rows));
}

std::optional<edt::desc_target> edt::play_preflight_publisher::target_of(const problem& row) const
{
	const auto it = std::ranges::find_if(m_last, [&row](const preflight_problem& p) { return same(p.item, row); });
	if (it == m_last.end()) return std::nullopt;
	return it->target;
}
