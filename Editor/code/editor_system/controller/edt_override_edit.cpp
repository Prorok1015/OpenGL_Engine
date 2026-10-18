#include "edt_override_edit.h"
#include "scn_glm_json_convert.h"
#include "desc_json_ops.h"
#include "edt_shared_state.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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
	scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target)
{
	std::vector<listed_node> out;
	auto path = compose_path(assembler, owner, target, false);
	if (!path) return out;

	std::vector<std::unique_ptr<prefab_node>> keep;
	out.reserve(path->composed->children.size());
	for (const auto& child : path->composed->children) {
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
	auto value = std::make_shared<const std::optional<prefab_node>>(find_base_node(m_assembler, owner, target));
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
	auto value = std::make_shared<const std::vector<listed_node>>(list_children(m_assembler, owner, target));
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
	const json::object& base, const json::object& edited)
{
	json::object changed = diff(base, edited);
	erase_records(owner, target, component);
	if (changed.empty()) return;

	scn::override_record record;
	record.target.assign(target.begin(), target.end());
	record.component = component;
	record.value = std::move(changed);
	owner.overrides.push_back(std::move(record));
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
