#include "edt_shape_request.h"
#include <format>

namespace json = boost::json;

namespace edt
{
	namespace
	{
		std::string failure_text(std::string_view command, const cmd::result& result)
		{
			return std::format("{}: {} ({})", command, result.error, result.code);
		}
	}

	std::string run_shape_request(action_registry& actions, std::string_view world, const mesh::shape_request& request)
	{
		const json::object parent = mesh::node_ref(world, request.parent);
		if (!request.base_shape.empty()) {
			const cmd::result added = actions.invoke("shape.add_base", mesh::add_base_args(parent, request.base_shape));
			return added.ok ? std::string{} : failure_text("shape.add_base", added);
		}

		const json::object* args = request.create_args.if_object();
		if (!args) return "the request names no shape";
		// shape.create writes three files; once it has, a refused node would leave them behind, so the parent is looked for first.
		const cmd::result found = actions.invoke("node.get", json::object{ { "ref", parent }, { "components", json::array{} } });
		if (!found.ok) return std::format("the node the shape goes under: {} ({})", found.error, found.code);
		const cmd::result created = actions.invoke("shape.create", *args);
		if (!created.ok) return failure_text("shape.create", created);
		const json::object* made = created.value.if_object();
		const json::string* poly = made && made->if_contains("poly") ? made->at("poly").if_string() : nullptr;
		if (!poly) return "shape.create returned no poly file";
		const cmd::result added = actions.invoke("shape.add", mesh::add_parametric_args(parent, std::string(*poly), std::string(args->at("name").as_string())));
		if (added.ok) return {};
		return failure_text("shape.add", added) + std::format("; the shape's files '{}' were written", std::string(*poly));
	}
}
