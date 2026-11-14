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

	// The engine's own protocols: a string naming one is meant as a reference even where no schema says so.
	bool is_engine_protocol(std::string_view protocol)
	{
		static constexpr std::string_view k_protocols[] = { "res", "engine", "memory", "user", "edt", "code", "cache", "mod" };
		return std::ranges::find(k_protocols, protocol) != std::end(k_protocols);
	}

	bool is_tag_kind(desc::field_kind kind)
	{
		return kind == desc::field_kind::res_tag || kind == desc::field_kind::desc_ref;
	}

	// A field the schema says holds a tag (or a list of them): its value is a reference, whatever it looks like.
	bool is_tag_field(const desc::field_schema& field)
	{
		return is_tag_kind(field.kind) || ((field.kind == desc::field_kind::array || field.kind == desc::field_kind::map) && is_tag_kind(field.element));
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

		void check_tag(const std::string& text, const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			if (text.empty()) return;
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

		// The strings of a tag field; an object there is an inline desc, not a reference.
		void check_tags(const json::value& value, const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			if (const auto* text = value.if_string()) {
				check_tag(std::string(text->data(), text->size()), path, json_path);
			} else if (const auto* array = value.if_array()) {
				for (std::size_t i = 0; i < array->size(); ++i) check_tags((*array)[i], path, std::format("{}/{}", json_path, i));
			}
		}

		// The values of a map of tags, by key.
		void check_tag_map(const json::value& value, const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			const auto* entries = value.if_object();
			if (!entries) return;
			for (const auto& entry : *entries) {
				check_tags(entry.value(), path, json_path + "/" + pointer_token(std::string_view(entry.key().data(), entry.key().size())));
			}
		}

		// A field of no known type may hold any text (a label, a web address): only a string naming one of the engine's
		// protocols is taken for a reference, and what is wrong with it (the rule of #697, a file that is not there) is a
		// warning; another scheme (https://) is left alone.
		void check_loose(const json::value& value, const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			if (const auto* text = value.if_string()) {
				const std::string_view view(text->data(), text->size());
				if (view.find("://") == std::string_view::npos) return;
				const res::tag reference{ std::string(view) };
				if (!is_engine_protocol(reference.protocol())) return;
				if (std::string why = res::reference_problem(reference, scene.level_tag.protocol()); !why.empty()) {
					add(edt::problem_severity::warning, "reference", why, path, json_path);
					return;
				}
				if (services.resources && !services.resources->exists(reference)) {
					add(edt::problem_severity::warning, "missing_resource", std::format("'{}' does not exist", view), path, json_path);
				}
			} else if (const auto* object = value.if_object()) {
				for (const auto& member : *object) {
					check_loose(member.value(), path, json_path + "/" + pointer_token(std::string_view(member.key().data(), member.key().size())));
				}
			} else if (const auto* array = value.if_array()) {
				for (std::size_t i = 0; i < array->size(); ++i) check_loose((*array)[i], path, std::format("{}/{}", json_path, i));
			}
		}

		void check_fields(const json::object& fields, const desc::type_schema* schema, const std::vector<std::uint64_t>& path,
			const std::string& json_path)
		{
			for (const auto& member : fields) {
				const std::string_view key(member.key().data(), member.key().size());
				if (key.starts_with("__")) continue;
				const std::string at = json_path + "/" + pointer_token(key);
				const desc::field_schema* field = schema ? schema->find_field(key) : nullptr;
				if (!field && schema) field = schema->find_field("/" + std::string(key));
				if (field && field->kind == desc::field_kind::map && is_tag_field(*field)) check_tag_map(member.value(), path, at);
				else if (field && is_tag_field(*field)) check_tags(member.value(), path, at);
				else check_loose(member.value(), path, at);
			}
		}

		void check_component(const std::string& key, const scn::prefab_desc::prefab_comp_node& component,
			const std::vector<std::uint64_t>& path, const std::string& json_path)
		{
			if (component.parent_desc.is_valid() && component.parent_desc.has_error()) {
				add(edt::problem_severity::error, "missing_resource", std::format("component '{}': its __parent '{}' did not load", key,
					component.parent_desc.get_tag().view()), path, json_path + "/__parent");
				if (component.type_name.empty()) return;
			}
			// `"key": "<tag>"` names no type: it is the type of the desc it refers to, as the assembler spawns it.
			std::string type = component.type_name;
			if (type.empty() && component.parent_desc.is_valid()) {
				// Still loading: the check waits for nothing, so there is no type to judge yet.
				if (!component.parent_desc.is_ready()) return;
				type = std::string(component.parent_desc->get_type());
			}
			if (type.empty()) type = key;
			if (services.descs && !services.descs->is_registered(type)) {
				add(edt::problem_severity::error, "unknown_component", std::format("component '{}': the type '{}' is not registered", key, type),
					path, json_path);
				return;
			}
			const desc::type_schema* schema = services.descs ? services.descs->find_schema(type) : nullptr;
			if (schema) {
				// What the desc system keeps beside the fields (__version, __type) is no field of the schema.
				json::object fields;
				for (const auto& member : component.overrides) {
					if (!member.key().starts_with("__")) fields.emplace(member.key(), member.value());
				}
				const desc::override_failure bad = desc::check_override_object(*schema, fields, nullptr, &services.descs->schemas());
				if (bad.status != desc::override_status::valid) {
					const std::string expected = desc::override_expectation(*schema, bad, &services.descs->schemas());
					add(edt::problem_severity::error, "schema", std::format("component '{}' ({}): {} ({}){}{}", key, type,
						std::string_view(bad.pointer).substr(bad.pointer.empty() ? 0 : 1),
						bad.status == desc::override_status::stale ? "no such field" : "the value does not fit",
						expected.empty() ? "" : ": expected ", expected), path, json_path + bad.pointer);
				}
			}
			if (services.assembler && !services.assembler->has_spawner(type)) {
				add(edt::problem_severity::warning, "no_spawner", std::format("component '{}': nothing spawns the type '{}', it makes no component",
					key, type), path, json_path);
			}
			check_fields(component.overrides, schema, path, json_path);
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
	return error_count(problems) != 0;
}

std::size_t edt::error_count(const std::vector<preflight_problem>& problems)
{
	return static_cast<std::size_t>(std::ranges::count_if(problems, [](const preflight_problem& p) { return p.item.severity == problem_severity::error; }));
}

json::object edt::problems_json(const std::vector<preflight_problem>& problems)
{
	json::array rows;
	rows.reserve(problems.size());
	for (const preflight_problem& p : problems) {
		json::object row;
		row["severity"] = p.item.severity == problem_severity::error ? "error" : "warning";
		row["code"] = json::string_view(p.item.code.data(), p.item.code.size());
		row["message"] = json::string_view(p.item.message.data(), p.item.message.size());
		row["world"] = json::string_view(p.target.world.data(), p.target.world.size());
		if (!p.target.path.empty()) {
			json::array path;
			for (const std::uint64_t id : p.target.path) {
				const std::string hex = std::format("{:016x}", id);
				path.emplace_back(json::string_view(hex.data(), hex.size()));
			}
			row["path"] = std::move(path);
		}
		if (!p.item.notes.empty()) row["json_path"] = json::string_view(p.item.notes.front().data(), p.item.notes.front().size());
		rows.emplace_back(std::move(row));
	}
	json::object value;
	value["problems"] = std::move(rows);
	return value;
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
