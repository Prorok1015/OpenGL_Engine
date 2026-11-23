#include "edt_prefab_shape.h"
#include "edt_prefab_document.h"
#include <algorithm>
#include <format>
#include <optional>

namespace json = boost::json;

namespace edt
{
	namespace
	{
		template <class Node>
		Node* node_at(Node& root, const std::vector<std::uint64_t>& path)
		{
			Node* node = &root;
			for (const std::uint64_t id : path) {
				const auto found = std::ranges::find(node->children, id, &scn::prefab_desc::prefab_node::node_id);
				if (found == node->children.end()) return nullptr;
				node = &*found;
			}
			return node;
		}

		std::string created_shape_node(action_registry& actions, const json::object& args, std::optional<scn::prefab_desc::prefab_node>& node)
		{
			const cmd::result created = actions.invoke("shape.create", args);
			if (!created.ok) return std::format("shape.create: {} ({})", created.error, created.code);
			const json::object* made = created.value.if_object();
			const std::optional<json::object> fields = made ? mesh::mesh_fields_of_created(*made) : std::nullopt;
			if (!fields) return "shape.create returned no mesh range";
			node = mesh::mesh_node(args.at("name").as_string(), *fields);
			return {};
		}
	}

	std::string add_shape_to_prefab(action_registry& actions, prefab_document& prefab, const mesh::shape_request& request)
	{
		// shape.create writes three files; once it has, a refused node would leave them behind, so the document and the parent are checked first.
		const scn::prefab_desc* working = prefab.working();
		if (!working || prefab.edits_locked()) return "the prefab cannot be edited now";
		if (!node_at(working->get_root(), request.parent)) return "the node the shape goes under is gone";

		std::optional<scn::prefab_desc::prefab_node> node;
		if (!request.base_shape.empty()) {
			const mesh::base_shape* shape = mesh::find_base_shape(request.base_shape);
			if (!shape) return std::format("no base shape '{}'", request.base_shape);
			node = mesh::base_shape_node(*shape);
		} else {
			const json::object* args = request.create_args.if_object();
			if (!args) return "the request names no shape";
			if (std::string error = created_shape_node(actions, *args, node); !error.empty()) return error;
		}

		bool found_parent = false;
		const bool changed = prefab.edit([&](scn::prefab_desc& copy) {
			scn::prefab_desc::prefab_node* parent = node_at(copy.get_root(), request.parent);
			if (!parent) return false;
			found_parent = true;
			parent->children.push_back(std::move(*node));
			return true;
		});
		if (changed) return {};
		return found_parent ? "the prefab cannot be edited now" : "the node the shape goes under is gone";
	}
}
