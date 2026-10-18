#pragma once
#include "level/scn_prefab_desc.h"
#include <boost/json.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

// How the editor turns an edit inside a prefab instance into override records on the instance node.
// `owner` is the node of the edited desc that instantiates the prefab, `target` the node-id path below it.
namespace edt::override_edit
{
	using prefab_node = scn::prefab_desc::prefab_node;

	// The prefab (loaded) that a node instantiates through one of its reference components, or null.
	const scn::prefab_desc* referenced_prefab(const prefab_node& node);

	// The node of the instantiated content that `target` addresses, composed the way reconcile builds it: records of
	// every prefab level on the way are applied, only the owner's own edits of this node are left out. Name, id,
	// placement and components are filled, children are not. Empty if the content has no such node.
	std::optional<prefab_node> find_base_node(scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target);

	// A node of an instance as the hierarchy lists it: no children, `has_children` says whether it has some.
	struct listed_node {
		prefab_node node;
		bool has_children = false;
	};

	// The children of the node `target` addresses (empty target: the owner itself), composed like find_base_node does,
	// with the owner's own records applied. For an owner these include the owner's own children.
	std::vector<listed_node> list_children(scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target);

	// Expanding an instance copies prefab content and logs about broken input, so the panels do it once per selection or
	// edit instead of once per frame: results are kept per (owner, target) until invalidate().
	// References handed out stay valid until the next begin_frame() after an invalidate().
	class view_cache
	{
	public:
		explicit view_cache(scn::ecs_assembler& assembler) : m_assembler(assembler) {}

		void invalidate();
		void begin_frame();
		const std::optional<prefab_node>& base(const prefab_node& owner, std::span<const uint64_t> target);
		const std::vector<listed_node>& children(const prefab_node& owner, std::span<const uint64_t> target);

	private:
		template<typename T>
		struct entry {
			const prefab_node* owner;
			std::vector<uint64_t> target;
			T value;
		};

		scn::ecs_assembler& m_assembler;
		std::deque<entry<std::optional<prefab_node>>> m_bases;
		std::deque<entry<std::vector<listed_node>>> m_children;
		std::vector<std::deque<entry<std::optional<prefab_node>>>> m_retired_bases;
		std::vector<std::deque<entry<std::vector<listed_node>>>> m_retired_children;
	};

	// A component as the base content defines it: the referenced desc's data with the component's own values and edits on top.
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
