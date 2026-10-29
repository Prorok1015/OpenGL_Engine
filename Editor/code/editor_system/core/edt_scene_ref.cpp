#include "edt_scene_ref.h"
#include "edt_scene_document.h"
#include <snk/desc/desc_node_id.h>
#include <boost/json/object.hpp>
#include <algorithm>
#include <format>

namespace {
	namespace json = boost::json;

	template<typename Scene, typename Node>
	bool locate_in(Scene& scene, const json::value& ref, edt::scene_ref::location<Node>& out, cmd::result& error)
	{
		const json::object* object = ref.if_object();
		const json::value* context = object ? object->if_contains("context") : nullptr;
		const json::value* path = object ? object->if_contains("path") : nullptr;
		if (!context || !context->is_string() || !path || !path->is_array()) {
			error = cmd::result::failure(cmd::codes::invalid_args, "a ref is {context: world name, path: [hex node id, ...]}");
			return false;
		}
		for (const json::value& item : path->as_array()) {
			const auto id = item.is_string() ? desc::parse_node_id(std::string_view(item.as_string())) : std::nullopt;
			if (!id) {
				error = cmd::result::failure(cmd::codes::invalid_args, "a path item is not a 16-char hex node id");
				return false;
			}
			out.path.push_back(*id);
		}
		const std::string_view name(context->as_string());
		const auto world = edt::scene_ref::find_world(scene, name);
		if (!world) {
			error = cmd::result::failure(cmd::codes::not_found, std::format("no world '{}' in the document", name));
			return false;
		}
		out.world = *world;
		out.world_name = scene.world_names[*world];
		auto& root = scene.world_descs[*world].get_root();
		auto* level = &root.children;
		for (std::size_t i = 0; i < out.path.size(); ++i) {
			const auto it = std::ranges::find(*level, out.path[i], &edt::scene_ref::prefab_node::node_id);
			if (it == level->end()) {
				if (!out.owner) {
					error = cmd::result::failure(cmd::codes::not_found, "no node at that path");
					return false;
				}
				out.target.assign(out.path.begin() + static_cast<std::ptrdiff_t>(i), out.path.end());
				break;
			}
			out.owner = &*it;
			level = &it->children;
		}
		if (!out.owner) {
			out.owner = &root;
			out.is_root = true;
		}
		return true;
	}
}

std::string edt::scene_ref::with_ref(std::string text)
{
	constexpr std::string_view placeholder = "@REF@";
	constexpr std::string_view ref = R"({"type":"object","properties":{"context":{"type":"string","description":"name of the world"},"path":{"type":"array","items":{"type":"string"},"description":"node ids from the world root, 16-char hex each"}},"required":["context","path"],"additionalProperties":false})";
	for (std::size_t at = text.find(placeholder); at != std::string::npos; at = text.find(placeholder, at + ref.size())) {
		text.replace(at, placeholder.size(), ref);
	}
	return text;
}

std::optional<std::size_t> edt::scene_ref::find_world(const scene_document& scene, std::string_view context)
{
	const auto index_of = [&scene](std::string_view name) -> std::optional<std::size_t> {
		const std::size_t count = std::min(scene.world_names.size(), scene.world_descs.size());
		for (std::size_t i = 0; i < count; ++i) {
			if (scene.world_names[i] == name) return i;
		}
		return std::nullopt;
	};
	auto index = index_of(context);
	if (!index) {
		const std::size_t slash = context.find('/');
		if (slash != std::string_view::npos) index = index_of(context.substr(slash + 1));
	}
	return index;
}

bool edt::scene_ref::locate(const scene_document& scene, const json::value& ref, location<const prefab_node>& out, cmd::result& error)
{
	return locate_in(scene, ref, out, error);
}

bool edt::scene_ref::locate(scene_document& scene, const json::value& ref, location<prefab_node>& out, cmd::result& error)
{
	return locate_in(scene, ref, out, error);
}
