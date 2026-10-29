#pragma once
#include <snk/command/cmd_types.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt { class scene_document; }

// A node named the way agents and scripts name it (A4): {context: world name, path: [16-char hex node ids]}. One
// reading of a ref for the commands that read the scene and the ones that change it.
namespace edt::scene_ref
{
	using prefab_node = scn::prefab_desc::prefab_node;

	// A world by name; a "<level>/<world>" context is read as its world.
	std::optional<std::size_t> find_world(const scene_document& scene, std::string_view context);

	template<typename Node>
	struct location
	{
		std::size_t world = 0;
		std::string world_name;
		// The deepest node of the world desc on the path; the world root for an empty path.
		Node* owner = nullptr;
		std::vector<std::uint64_t> path;
		// The ids below `owner`, inside the prefab it instantiates.
		std::vector<std::uint64_t> target;
		bool is_root = false;
	};

	// `text` (a JSON Schema) with every "@REF@" replaced by the schema of a ref.
	std::string with_ref(std::string text);

	// False with `error` filled when the ref is malformed or leads nowhere.
	bool locate(const scene_document& scene, const boost::json::value& ref, location<const prefab_node>& out, cmd::result& error);
	bool locate(scene_document& scene, const boost::json::value& ref, location<prefab_node>& out, cmd::result& error);
}
