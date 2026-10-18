#include "edt_override_edit.h"
#include "scn_glm_json_convert.h"
#include "desc_json_ops.h"
#include <algorithm>
#include <ranges>

namespace json = boost::json;

namespace
{
	using prefab_node = scn::prefab_desc::prefab_node;
	constexpr int max_nesting = 16;

	bool same_target(const scn::override_record& record, std::span<const uint64_t> target, const std::string& component)
	{
		return record.component == component && std::ranges::equal(record.target, target);
	}

	const prefab_node* find_in(const prefab_node& node, uint64_t id, int depth)
	{
		if (auto child = std::ranges::find(node.children, id, &prefab_node::node_id); child != node.children.end()) {
			return &*child;
		}
		if (depth >= max_nesting) {
			return nullptr;
		}
		if (const auto* prefab = edt::override_edit::referenced_prefab(node)) {
			return find_in(prefab->get_root(), id, depth + 1);
		}
		return nullptr;
	}

	bool almost_equal(const glm::vec3& a, const glm::vec3& b)
	{
		return glm::length(a - b) < 1e-4f;
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

const edt::override_edit::prefab_node* edt::override_edit::find_base_node(const prefab_node& owner, std::span<const uint64_t> target)
{
	const prefab_node* node = &owner;
	for (uint64_t id : target) {
		node = find_in(*node, id, 0);
		if (!node) return nullptr;
	}
	return node;
}

json::object edt::override_edit::component_json(const scn::prefab_desc::prefab_comp_node& comp)
{
	json::object out;
	if (comp.parent_desc.is_valid() && comp.parent_desc.is_ready()) {
		comp.parent_desc->serialize(out);
	}
	desc::json_deep_merge(out, comp.overrides);
	return out;
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
		} else if (!before || *before != member.value()) {
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
	if (!almost_equal(rotation, base.rotation)) changed["rotation"] = json::value_from(rotation);
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
