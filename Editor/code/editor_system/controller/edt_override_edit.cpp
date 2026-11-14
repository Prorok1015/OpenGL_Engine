#include "edt_override_edit.h"
#include "edt_reference.h"
#include <snk/scene/scn_glm_json_convert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/scene/level/scn_override_record.h>
#include <snk/scene/level/scn_world_desc.h>
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <ranges>
#include <vector>

namespace json = boost::json;

namespace
{
	using prefab_node = scn::prefab_desc::prefab_node;

	bool same_target(const scn::override_record& record, std::span<const uint64_t> target, const std::string& component)
	{
		// remove / add_node records are not edits of the inspector and must survive its writes and resets
		return record.op == scn::override_op::set && record.component == component && std::ranges::equal(record.target, target);
	}

	bool almost_equal(const glm::vec3& a, const glm::vec3& b)
	{
		return glm::length(a - b) < 1e-4f;
	}

	// Euler triples such as 270 and -90 are one orientation; compare orientations, not angles.
	bool same_rotation(const glm::vec3& a, const glm::vec3& b)
	{
		auto orientation = [] (const glm::vec3& deg) {
			return glm::quat_cast(glm::yawPitchRoll(glm::radians(deg.y), glm::radians(deg.x), glm::radians(deg.z)));
		};
		return std::abs(glm::dot(orientation(a), orientation(b))) > 1.0f - 1e-5f;
	}

	bool same_value(const json::value& a, const json::value& b)
	{
		if (a.is_number() && b.is_number()) {
			return a.to_number<double>() == b.to_number<double>();
		}
		return a == b;
	}

	void erase_records(prefab_node& owner, std::span<const uint64_t> target, const std::string& component)
	{
		std::erase_if(owner.overrides, [&] (const scn::override_record& r) { return same_target(r, target, component); });
	}

	// Inline descs inside inline descs: deeper than any shipped data.
	constexpr int k_max_depth = 8;

	bool is_reference(const json::value& value)
	{
		return edt::is_reference_object(value) && edt::reference_object_problem(value.as_object()).empty();
	}

	bool is_tag_kind(desc::field_kind kind)
	{
		return kind == desc::field_kind::res_tag || kind == desc::field_kind::desc_ref;
	}

	std::string pointer_of(std::string_view key)
	{
		return key.starts_with('/') ? std::string(key) : "/" + std::string(key);
	}

	std::string token_of(const json::string& key)
	{
		return res::json_pointer_token(std::string_view(key.data(), key.size()));
	}

	// `find` gives the schema of a type name: a desc system's, or a registry's.
	template<class Find>
	void collect_whole(const Find& find, const desc::type_schema& schema, const json::value& base, const json::value& edited,
		const std::string& at, std::vector<std::string>& out, int depth)
	{
		if (depth >= k_max_depth) return;
		const auto whole_or_into = [&](const json::value& held, const std::string& where, const auto& into) {
			const json::value* was = desc::json_pointer_get(base, where);
			if (is_reference(held) || (was && is_reference(*was))) {
				out.push_back(where);
				return;
			}
			into();
		};
		for (const desc::field_schema& field : schema.fields) {
			std::vector<std::string> pointers{ pointer_of(field.key) };
			for (const std::string& alias : field.read_aliases) pointers.push_back(pointer_of(alias));
			for (const std::string& pointer : pointers) {
				const std::string where = at + pointer;
				const json::value* held = desc::json_pointer_get(edited, where);
				if (!held || !held->is_object()) continue;
				if (is_tag_kind(field.kind)) {
					whole_or_into(*held, where, [&] {
						const json::value* type = held->as_object().if_contains("__type");
						const desc::type_schema* nested = type && type->is_string() ? find(std::string_view(type->as_string())) : nullptr;
						if (nested) collect_whole(find, *nested, base, edited, where, out, depth + 1);
					});
				} else if (field.kind == desc::field_kind::structure) {
					if (const desc::type_schema* nested = find(field.ref_type)) collect_whole(find, *nested, base, edited, where, out, depth + 1);
				} else if (field.kind == desc::field_kind::map) {
					const desc::type_schema* nested = field.element == desc::field_kind::structure ? find(field.ref_type) : nullptr;
					for (const auto& entry : held->as_object()) {
						if (!entry.value().is_object()) continue;
						const std::string item = where + "/" + token_of(entry.key());
						if (is_tag_kind(field.element)) whole_or_into(entry.value(), item, [] {});
						else if (nested) collect_whole(find, *nested, base, edited, item, out, depth + 1);
					}
				}
			}
		}
	}
}

namespace
{
	// The written worlds follow the edited ones node by node.
	class record_references
	{
	public:
		using prefab_node = scn::prefab_desc::prefab_node;

		record_references(const desc::desc_system& descs, scn::ecs_assembler& assembler) : m_descs(descs), m_assembler(assembler) {}

		void node(const prefab_node& edited, boost::json::object& written)
		{
			if (boost::json::value* records = written.if_contains("overrides"); records && records->is_array()) {
				for (boost::json::value& item : records->as_array()) {
					record(edited, item);
				}
			}
			boost::json::value* children = written.if_contains("children");
			if (!children || !children->is_array() || children->as_array().size() != edited.children.size()) return;
			for (std::size_t i = 0; i < edited.children.size(); ++i) {
				if (boost::json::object* child = children->as_array()[i].if_object()) node(edited.children[i], *child);
			}
		}

	private:
		static std::string type_of(const scn::prefab_desc::prefab_comp_node& component)
		{
			if (!component.type_name.empty()) return component.type_name;
			if (!component.parent_desc.is_valid()) return {};
			component.parent_desc.wait();
			return component.parent_desc.is_ready() ? std::string(component.parent_desc->get_type()) : std::string{};
		}

		const std::optional<prefab_node>& base_of(const prefab_node& owner, const std::vector<std::uint64_t>& target)
		{
			auto key = std::make_pair(&owner, target);
			auto found = m_bases.find(key);
			if (found == m_bases.end()) {
				found = m_bases.emplace(std::move(key), edt::override_edit::find_base_node(m_assembler, owner, target)).first;
			}
			return found->second;
		}

		void record(const prefab_node& owner, boost::json::value& written)
		{
			const auto parsed = scn::parse_override_record(written);
			boost::json::value* value = written.is_object() ? written.as_object().if_contains("value") : nullptr;
			if (!parsed || !value || parsed->op != scn::override_op::set || parsed->component.empty()
				|| parsed->component == scn::transform_pseudo_component) {
				return;
			}
			const std::optional<prefab_node>& base = base_of(owner, parsed->target);
			if (!base) return;
			const auto component = base->components.find(parsed->component);
			if (component == base->components.end()) return;
			const std::string type = type_of(component->second);
			if (type.empty()) {
				egLOG_WARN("Editor/Save", "override record of component '{}' at '{}': the component's desc did not load, its references are written as they are",
					parsed->component, parsed->path);
				return;
			}
			const desc::type_schema* schema = m_descs.find_schema(type);
			if (!schema) return;
			if (!parsed->path.empty()) {
				m_descs.write_references(*schema, parsed->path, *value);
				return;
			}
			// A record of the whole component is merged into it.
			const boost::json::value merged_into(edt::override_edit::component_json(component->second));
			m_descs.write_references(*schema, {}, *value, &merged_into);
		}

		const desc::desc_system& m_descs;
		scn::ecs_assembler& m_assembler;
		std::map<std::pair<const prefab_node*, std::vector<std::uint64_t>>, std::optional<prefab_node>> m_bases;
	};

}

void edt::override_edit::write_record_references(const desc::desc_system& descs, scn::ecs_assembler& assembler,
	const std::vector<scn::world_desc>& worlds, json::value& level_file)
{
	json::value* written = level_file.is_object() ? level_file.as_object().if_contains("worlds") : nullptr;
	// A level saved over its parent may leave the worlds out.
	if (!written || !written->is_array() || written->as_array().size() != worlds.size()) return;
	record_references records(descs, assembler);
	for (std::size_t i = 0; i < worlds.size(); ++i) {
		if (json::object* world = written->as_array()[i].if_object()) records.node(worlds[i].get_root(), *world);
	}
}

std::vector<std::string> edt::override_edit::whole_values(const desc::desc_system& descs, const desc::type_schema& schema,
	const json::value& base, const json::value& edited)
{
	std::vector<std::string> out;
	collect_whole([&descs](std::string_view name) { return descs.find_schema(name); }, schema, base, edited, {}, out, 0);
	return out;
}

void edt::override_edit::merge_layer(const desc::schema_registry& schemas, const desc::type_schema& schema, json::object& into,
	const json::object& layer)
{
	std::vector<std::string> whole;
	collect_whole([&schemas](std::string_view name) { return schemas.find(name); }, schema, json::value(into), json::value(layer), {}, whole, 0);
	desc::json_deep_merge(into, layer);
	if (whole.empty()) return;
	json::value merged(std::move(into));
	const json::value given(layer);
	for (const std::string& pointer : whole) {
		if (const json::value* value = desc::json_pointer_get(given, pointer)) desc::json_pointer_set(merged, pointer, *value);
	}
	into = std::move(merged.as_object());
}

const scn::prefab_desc* edt::override_edit::referenced_prefab(const prefab_node& node)
{
	for (const auto& [key, comp] : node.components) {
		if (!comp.parent_desc.is_valid() || !comp.parent_desc.is_ready()) continue;
		if (comp.parent_desc->get_type() != scn::prefab_desc::__type) continue;
		auto ptr = comp.parent_desc.get();
		if (ptr) return static_cast<const scn::prefab_desc*>(ptr.get());
	}
	return nullptr;
}

namespace
{
	// The nodes on the way to a target, each expanded the way reconcile does it: records of intermediate prefabs
	// land in the nodes below them. Heap-owned, so the pointers survive moving this around.
	struct composed_path
	{
		std::unique_ptr<prefab_node> copy;
		std::vector<std::unique_ptr<prefab_node>> expansions;
		const prefab_node* raw = nullptr;      // the node as it is stored (inside copy or an expansion)
		const prefab_node* composed = nullptr; // the same node expanded
	};

	const prefab_node* expand_node(scn::ecs_assembler& assembler, const prefab_node& raw, std::vector<std::unique_ptr<prefab_node>>& keep)
	{
		auto expanded = std::make_unique<prefab_node>();
		res::tag source;
		if (!scn::expand_instance(assembler, raw, *expanded, source)) return &raw;
		keep.push_back(std::move(expanded));
		return keep.back().get();
	}

	// `without_target_edits`: leave out the owner's own set records of the target node, the edit a base is for.
	std::optional<composed_path> compose_path(
		scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target, bool without_target_edits)
	{
		composed_path path;
		path.copy = std::make_unique<prefab_node>(owner);
		if (without_target_edits) {
			std::erase_if(path.copy->overrides, [&] (const scn::override_record& r) {
				return r.op == scn::override_op::set && std::ranges::equal(r.target, target);
			});
		}
		path.raw = path.copy.get();
		path.composed = expand_node(assembler, *path.raw, path.expansions);
		for (uint64_t id : target) {
			auto child = std::ranges::find(path.composed->children, id, &prefab_node::node_id);
			if (child == path.composed->children.end()) return std::nullopt;
			path.raw = &*child;
			path.composed = expand_node(assembler, *path.raw, path.expansions);
		}
		return path;
	}
}

std::vector<edt::override_edit::listed_node> edt::override_edit::list_children(
	scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target, std::size_t limit, std::size_t* total)
{
	std::vector<listed_node> out;
	if (total) *total = 0;
	auto path = compose_path(assembler, owner, target, false);
	if (!path) return out;

	if (total) *total = path->composed->children.size();
	std::vector<std::unique_ptr<prefab_node>> keep;
	out.reserve(std::min(path->composed->children.size(), limit));
	for (const auto& child : path->composed->children) {
		if (out.size() >= limit) break;
		const prefab_node* expanded = expand_node(assembler, child, keep);
		listed_node listed;
		listed.node.name = child.name;
		listed.node.node_id = child.node_id;
		listed.node.components = expanded->components;
		listed.has_children = !expanded->children.empty();
		out.push_back(std::move(listed));
	}
	return out;
}

std::optional<edt::override_edit::prefab_node> edt::override_edit::find_base_node(
	scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target)
{
	auto path = compose_path(assembler, owner, target, true);
	if (!path) return std::nullopt;
	const prefab_node* raw = path->raw;
	const prefab_node* composed = path->composed;

	// An instance node's own transform places it when it spells one out, otherwise the prefab root's does.
	const prefab_node& placed = raw->has_transform ? *raw : *composed;
	prefab_node base;
	base.name = raw->name;
	base.node_id = raw->node_id;
	base.position = placed.position;
	base.rotation = placed.rotation;
	base.scale = placed.scale;
	base.has_transform = true;
	base.components = composed->components;
	return base;
}

void edt::override_edit::view_cache::invalidate()
{
	m_bases.clear();
	m_children.clear();
}

void edt::override_edit::view_cache::sync_generation()
{
	if (m_generation == m_state.desc_generation) return;
	invalidate();
	m_generation = m_state.desc_generation;
}

std::shared_ptr<const std::optional<edt::override_edit::prefab_node>> edt::override_edit::view_cache::base(
	const prefab_node& owner, std::span<const uint64_t> target)
{
	sync_generation();
	for (const auto& e : m_bases) {
		if (e.world == m_state.active_world_idx && e.owner_id == owner.node_id && std::ranges::equal(e.target, target)) return e.value;
	}
	auto value = std::make_shared<const std::optional<prefab_node>>(find_base_node(*m_services.assembler(), owner, target));
	m_bases.push_back({ m_state.active_world_idx, owner.node_id, std::vector<uint64_t>(target.begin(), target.end()), value });
	return value;
}

std::shared_ptr<const std::vector<edt::override_edit::listed_node>> edt::override_edit::view_cache::children(
	const prefab_node& owner, std::span<const uint64_t> target)
{
	sync_generation();
	for (const auto& e : m_children) {
		if (e.world == m_state.active_world_idx && e.owner_id == owner.node_id && std::ranges::equal(e.target, target)) return e.value;
	}
	auto value = std::make_shared<const std::vector<listed_node>>(list_children(*m_services.assembler(), owner, target));
	m_children.push_back({ m_state.active_world_idx, owner.node_id, std::vector<uint64_t>(target.begin(), target.end()), value });
	return value;
}

json::object edt::override_edit::component_json(const scn::prefab_desc::prefab_comp_node& comp)
{
	if (comp.parent_desc.is_valid() && comp.parent_desc.is_ready()) {
		auto parent = comp.parent_desc.get();
		return scn::compose_component_json(comp, parent.get());
	}
	return scn::compose_component_json(comp, nullptr);
}

json::object edt::override_edit::diff(const json::object& base, const json::object& edited)
{
	json::object out;
	for (const auto& member : edited) {
		if (member.key().starts_with("__")) continue;
		const auto* before = base.if_contains(member.key());
		if (before && before->is_object() && member.value().is_object()) {
			json::object nested = diff(before->as_object(), member.value().as_object());
			if (!nested.empty()) out.insert_or_assign(member.key(), std::move(nested));
		} else if (!before || !same_value(*before, member.value())) {
			out.insert_or_assign(member.key(), member.value());
		}
	}
	return out;
}

json::object edt::override_edit::effective_component(
	const prefab_node& owner, std::span<const uint64_t> target, const std::string& component, json::object base)
{
	json::value holder = std::move(base);
	for (const auto& record : owner.overrides) {
		if (record.op != scn::override_op::set || !same_target(record, target, component)) continue;
		if (record.path.empty()) {
			if (record.value.is_object()) desc::json_deep_merge(holder.as_object(), record.value.as_object());
		} else {
			desc::json_pointer_set(holder, record.path, record.value);
		}
	}
	return std::move(holder.as_object());
}

void edt::override_edit::effective_transform(
	const prefab_node& owner, std::span<const uint64_t> target, const prefab_node& base,
	glm::vec3& position, glm::vec3& rotation, glm::vec3& scale)
{
	json::value holder = json::object{
		{ "position", json::value_from(base.position) },
		{ "rotation", json::value_from(base.rotation) },
		{ "scale", json::value_from(base.scale) },
	};
	for (const auto& record : owner.overrides) {
		if (record.op != scn::override_op::set || !same_target(record, target, std::string{ scn::transform_pseudo_component })) continue;
		if (record.path.empty()) {
			if (record.value.is_object()) desc::json_deep_merge(holder.as_object(), record.value.as_object());
		} else {
			desc::json_pointer_set(holder, record.path, record.value);
		}
	}
	try {
		const auto& t = holder.as_object();
		position = json::value_to<glm::vec3>(t.at("position"));
		rotation = json::value_to<glm::vec3>(t.at("rotation"));
		scale = json::value_to<glm::vec3>(t.at("scale"));
	} catch (const std::exception&) {
		position = base.position;
		rotation = base.rotation;
		scale = base.scale;
	}
}

void edt::override_edit::write_component(
	prefab_node& owner, std::span<const uint64_t> target, const std::string& component,
	const json::object& base, const json::object& edited, const desc::desc_system* descs, const desc::type_schema* schema)
{
	json::value changed = diff(base, edited);
	erase_records(owner, target, component);

	std::vector<scn::override_record> whole;
	const json::value base_value(base);
	const json::value edited_value(edited);
	const std::vector<std::string> replaced = descs && schema ? whole_values(*descs, *schema, base_value, edited_value) : std::vector<std::string>{};
	for (const std::string& pointer : replaced) {
		const json::value* now = desc::json_pointer_get(edited_value, pointer);
		if (!now) continue;
		desc::json_pointer_remove(changed, pointer);
		// The objects the removal left empty on the way say nothing.
		for (std::string parent = pointer.substr(0, pointer.rfind('/')); !parent.empty(); parent = parent.substr(0, parent.rfind('/'))) {
			const json::value* left = desc::json_pointer_get(changed, parent);
			if (!left || !left->is_object() || !left->as_object().empty()) break;
			desc::json_pointer_remove(changed, parent);
		}
		const json::value* was = desc::json_pointer_get(base_value, pointer);
		if (was && same_value(*was, *now)) continue;
		scn::override_record record;
		record.target.assign(target.begin(), target.end());
		record.component = component;
		record.path = pointer;
		record.value = *now;
		whole.push_back(std::move(record));
	}

	if (!changed.as_object().empty()) {
		scn::override_record record;
		record.target.assign(target.begin(), target.end());
		record.component = component;
		record.value = std::move(changed);
		owner.overrides.push_back(std::move(record));
	}
	// After the record of the whole component: applied later, each sets its value over what that one merged.
	for (scn::override_record& record : whole) owner.overrides.push_back(std::move(record));
}

void edt::override_edit::write_transform(
	prefab_node& owner, std::span<const uint64_t> target, const prefab_node& base,
	glm::vec3 position, glm::vec3 rotation, glm::vec3 scale)
{
	const std::string component{ scn::transform_pseudo_component };
	json::object changed;
	if (!almost_equal(position, base.position)) changed["position"] = json::value_from(position);
	if (!same_rotation(rotation, base.rotation)) changed["rotation"] = json::value_from(rotation);
	if (!almost_equal(scale, base.scale)) changed["scale"] = json::value_from(scale);

	erase_records(owner, target, component);
	if (changed.empty()) return;

	scn::override_record record;
	record.target.assign(target.begin(), target.end());
	record.component = component;
	record.value = std::move(changed);
	owner.overrides.push_back(std::move(record));
}

bool edt::override_edit::has_record(const prefab_node& owner, std::span<const uint64_t> target, const std::string& component)
{
	return std::ranges::any_of(owner.overrides, [&] (const scn::override_record& r) { return same_target(r, target, component); });
}

bool edt::override_edit::reset(prefab_node& owner, std::span<const uint64_t> target, const std::string& component)
{
	const bool had = has_record(owner, target, component);
	erase_records(owner, target, component);
	return had;
}
