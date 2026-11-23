#include "edt_shape_parameters.h"
#include "edt_mesh_io.h"
#include "edt_mesh_shape_asset.h"
#include <format>

namespace json = boost::json;

namespace edt::mesh
{
	namespace
	{
		constexpr std::string_view geometry_suffix = ".geom.desc";
		constexpr std::string_view protocol = "res://";

		std::string_view without_protocol(std::string_view tag)
		{
			return tag.starts_with(protocol) ? tag.substr(protocol.size()) : tag;
		}

		bool draws(const scn::prefab_desc::prefab_node& node, std::string_view geometry, const json::object* range, bool* differs)
		{
			for (const auto& [key, component] : node.components) {
				if (component.type_name != "mesh_node_desc") continue;
				const json::value* tag = component.overrides.if_contains("geometry");
				if (!tag || !tag->is_string() || without_protocol(std::string_view(tag->as_string())) != geometry) continue;
				if (range && differs) {
					for (const auto& [field, value] : *range) {
						const json::value* have = component.overrides.if_contains(field);
						if (!have || *have != value) *differs = true;
					}
				}
				return true;
			}
			return false;
		}
	}

	std::string poly_path_of_geometry(std::string_view geometry)
	{
		const std::string_view path = without_protocol(geometry);
		if (!path.ends_with(geometry_suffix) || path.size() == geometry_suffix.size()) return {};
		return std::string(path.substr(0, path.size() - geometry_suffix.size())) + std::string(poly_file_suffix);
	}

	shape_parameters load_shape_parameters(std::string poly_path, const desc_file_read& file)
	{
		shape_parameters out;
		out.poly = std::move(poly_path);
		if (file.missing || !file.located) {
			out.reason = "this mesh is not a shape made by the editor";
			return out;
		}
		if (!file.error.empty()) {
			out.what = shape_parameters::state::unreadable;
			out.reason = file.error;
			return out;
		}
		if (!file.top_layer) {
			out.what = shape_parameters::state::lower_layer;
			out.reason = "this shape belongs to a lower layer; it is not rebuilt from here";
			return out;
		}
		out.version = file.version;
		const std::string text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
		const shape_source_check source = check_shape_source(text);
		if (source.state != shape_source::generated) {
			out.what = source.state == shape_source::edited ? shape_parameters::state::edited_by_hand : shape_parameters::state::unreadable;
			out.reason = source.state == shape_source::edited ? "the mesh was edited by hand: it is not rebuilt from parameters" : source.error;
			return out;
		}
		out.kind = std::string(shape_kind_name(*source.generator));
		out.params = shape_params_to_json(*source.generator);
		out.what = shape_parameters::state::editable;
		return out;
	}

	std::size_t count_shape_nodes(const scn::prefab_desc::prefab_node& root, std::string_view geometry)
	{
		std::size_t count = draws(root, without_protocol(geometry), nullptr, nullptr) ? 1u : 0u;
		for (const auto& child : root.children) count += count_shape_nodes(child, geometry);
		return count;
	}

	std::size_t retarget_shape_nodes(scn::prefab_desc::prefab_node& root, std::string_view geometry, const json::object& range)
	{
		const std::string_view wanted = without_protocol(geometry);
		std::size_t changed = 0;
		bool differs = false;
		if (draws(root, wanted, &range, &differs) && differs) {
			for (auto& [key, component] : root.components) {
				if (component.type_name != "mesh_node_desc") continue;
				const json::value* tag = component.overrides.if_contains("geometry");
				if (!tag || !tag->is_string() || without_protocol(std::string_view(tag->as_string())) != wanted) continue;
				for (const auto& [field, value] : range) component.overrides[field] = value;
			}
			++changed;
		}
		for (auto& child : root.children) changed += retarget_shape_nodes(child, geometry, range);
		return changed;
	}

	std::optional<json::object> range_of_rebuilt(const json::object& answer)
	{
		json::object range;
		for (const char* field : { "vx_begin", "vx_end", "ind_begin", "ind_end" }) {
			const json::value* value = answer.if_contains(field);
			if (!value) return std::nullopt;
			range[field] = *value;
		}
		return range;
	}

	std::string rebuild_confirmation(const rebuild_facts& facts)
	{
		std::string text = std::format("Rebuild the shape '{}' from these parameters?\n\nBoth its files are written again.", facts.poly);
		if (facts.level_open) text += std::format("\n{} mesh node(s) of the open level take the new range.", facts.level_nodes);
		if (facts.prefab_open) text += std::format("\n{} mesh node(s) of the open prefab take the new range.", facts.prefab_nodes);
		text += "\nNodes in files that are not open keep the old range (they show it in Problems later, #1140).";
		text += "\n\nRebuild cannot be undone; the level's undo history will be cleared.";
		if (facts.level_open && facts.level_dirty) text += "\nThe level has unsaved changes; they stay unsaved.";
		if (facts.prefab_open && facts.prefab_dirty) text += "\nThe prefab has unsaved edits; the rebuild changes its working copy on top of them.";
		return text;
	}

	json::object rebuild_arguments(const shape_parameters& shape, const json::object& params)
	{
		return json::object{ { "path", shape.poly }, { "version", shape.version }, { "kind", shape.kind }, { "params", params } };
	}
}
