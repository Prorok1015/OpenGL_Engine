#include "edt_desc_location.h"
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace {
	// Takes the next "/<token>" off the front of `pointer`.
	std::optional<std::string_view> next_token(std::string_view& pointer)
	{
		if (!pointer.starts_with('/')) return std::nullopt;
		pointer.remove_prefix(1);
		const std::size_t end = std::min(pointer.find('/'), pointer.size());
		const std::string_view token = pointer.substr(0, end);
		pointer.remove_prefix(end);
		return token;
	}

	std::optional<std::size_t> next_index(std::string_view& pointer)
	{
		const auto token = next_token(pointer);
		if (!token || token->empty()) return std::nullopt;
		std::size_t index = 0;
		const auto [end, error] = std::from_chars(token->data(), token->data() + token->size(), index);
		if (error != std::errc{} || end != token->data() + token->size()) return std::nullopt;
		return index;
	}

	void walk_children(const scn::prefab_desc::prefab_node* node, std::string_view pointer, std::vector<std::uint64_t>& path)
	{
		while (node) {
			const auto name = next_token(pointer);
			if (!name || *name != "children") return;
			const auto index = next_index(pointer);
			if (!index || *index >= node->children.size()) return;
			node = &node->children[*index];
			if (node->node_id == 0) return;
			path.push_back(node->node_id);
		}
	}
}

edt::level_worlds edt::level_worlds_of(const scn::level_desc& desc, std::string level, std::vector<std::string> names)
{
	level_worlds out;
	out.level = std::move(level);
	out.names = std::move(names);
	for (const auto& handle : desc.get_worlds()) {
		level_worlds::world entry;
		entry.file = handle.get_tag().string();
		if (handle.is_ready()) {
			if (const auto world = handle.get_sync()) {
				entry.name = std::string(world->get_name());
				entry.root = &world->get_root();
			}
		}
		out.worlds.push_back(std::move(entry));
	}
	return out;
}

edt::desc_target edt::locate_desc(const level_worlds& level, std::string_view resource, std::string_view json_pointer)
{
	desc_target out;
	const auto in_world = [&](const level_worlds::world& world, std::string_view rest) {
		out.level = level.level;
		out.world = world.name;
		walk_children(world.root, rest, out.path);
	};

	if (std::ranges::find(level.names, resource) != level.names.end()) {
		std::string_view rest = json_pointer;
		const auto first = next_token(rest);
		if (first && *first == "worlds") {
			if (const auto index = next_index(rest); index && *index < level.worlds.size() && !level.worlds[*index].name.empty()) {
				in_world(level.worlds[*index], rest);
				return out;
			}
		}
		out.resource = level.level;
		return out;
	}
	for (const auto& world : level.worlds) {
		if (!world.file.empty() && world.file == resource && !world.name.empty()) {
			in_world(world, json_pointer);
			return out;
		}
	}
	out.resource = std::string(resource);
	return out;
}
