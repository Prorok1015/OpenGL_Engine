#pragma once
#include "level/scn_prefab_desc.h"
#include <boost/json.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <span>
#include <string>

// How the editor turns an edit inside a prefab instance into override records on the instance node.
// `owner` is the node of the edited desc that instantiates the prefab, `target` the node-id path below it.
namespace edt::override_edit
{
	using prefab_node = scn::prefab_desc::prefab_node;

	// The prefab (loaded) that a node instantiates through one of its reference components, or null.
	const scn::prefab_desc* referenced_prefab(const prefab_node& node);

	// The node of the instantiated content that `target` addresses, or null if the content has no such node.
	const prefab_node* find_base_node(const prefab_node& owner, std::span<const uint64_t> target);

	// A component as the base content defines it: the referenced desc's data with the component's own values on top.
	boost::json::object component_json(const scn::prefab_desc::prefab_comp_node& comp);

	// What `edited` changes compared to `base`: objects are compared key by key, everything else as a whole.
	boost::json::object diff(const boost::json::object& base, const boost::json::object& edited);

	// `base` with the owner's records for (target, component) applied: what the instance really has.
	boost::json::object effective_component(
		const prefab_node& owner, std::span<const uint64_t> target, const std::string& component, boost::json::object base);

	void effective_transform(
		const prefab_node& owner, std::span<const uint64_t> target, const prefab_node& base,
		glm::vec3& position, glm::vec3& rotation, glm::vec3& scale);

	// Replace the records of (target, component) by one record holding the difference to the base; no difference, no record.
	void write_component(
		prefab_node& owner, std::span<const uint64_t> target, const std::string& component,
		const boost::json::object& base, const boost::json::object& edited);

	void write_transform(
		prefab_node& owner, std::span<const uint64_t> target, const prefab_node& base,
		glm::vec3 position, glm::vec3 rotation, glm::vec3 scale);

	bool has_record(const prefab_node& owner, std::span<const uint64_t> target, const std::string& component);

	// "Reset to base": drops the records of (target, component). True if there were any.
	bool reset(prefab_node& owner, std::span<const uint64_t> target, const std::string& component);
}
