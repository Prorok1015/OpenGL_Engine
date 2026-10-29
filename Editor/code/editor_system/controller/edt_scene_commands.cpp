#include "edt_scene_commands.h"
#include "edt_override_edit.h"
#include "edt_scene_document.h"
#include "edt_scene_ref.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <boost/json.hpp>
#include <glm/vec3.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	namespace json = boost::json;
	using prefab_node = scn::prefab_desc::prefab_node;
	using mutable_location = edt::scene_ref::location<prefab_node>;

	constexpr std::string_view inside_instance = "inside a prefab instance only component.set and transform.set work";

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(edt::scene_ref::with_ref(std::string(text)), ec);
		ASSERT_MSG(!ec, "a scene command schema is not JSON: {}", ec.message());
		return parsed;
	}

	cmd::result failure(const char* code, std::string message)
	{
		return cmd::result::failure(code, std::move(message));
	}

	// RFC 7386: objects merge key by key, null removes the key, anything else replaces.
	json::value merge_patch(json::value target, const json::value& patch)
	{
		if (!patch.is_object()) return patch;
		if (!target.is_object()) target = json::object{};
		json::object& object = target.as_object();
		for (const auto& [key, value] : patch.as_object()) {
			if (value.is_null()) {
				object.erase(key);
				continue;
			}
			const json::value* current = object.if_contains(key);
			object[key] = merge_patch(current ? *current : json::value{}, value);
		}
		return target;
	}

	std::string pointer_token(std::string_view key)
	{
		std::string out;
		out.reserve(key.size());
		for (const char c : key) {
			if (c == '~') out += "~0";
			else if (c == '/') out += "~1";
			else out += c;
		}
		return out;
	}

	// Every field the patch sets (null clears it, which always fits) must be a field of the type and fit it.
	std::optional<cmd::result> check_fields(const desc::desc_system& descs, const std::string& type, const json::object& patch,
		const json::object& edited)
	{
		const desc::type_schema* schema = descs.find_schema(type);
		if (!schema) {
			return failure(cmd::codes::invalid_args, std::format("'{}' has no schema: its fields cannot be checked", type));
		}
		std::string refused;
		for (const auto& [key, value] : patch) {
			if (value.is_null()) continue;
			const json::value* now = edited.if_contains(key);
			const desc::json_change change{ "/" + pointer_token(key), now ? *now : value };
			const auto status = desc::validate_override(*schema, change, &descs.schemas());
			if (status == desc::override_status::valid) continue;
			refused += std::format("{}{} ({})", refused.empty() ? "" : ", ", std::string_view(key),
				status == desc::override_status::stale ? "no such field" : "the value does not fit");
		}
		if (!refused.empty()) return failure(cmd::codes::invalid_args, std::format("'{}' refuses: {}", type, refused));
		return std::nullopt;
	}

	std::optional<glm::vec3> vec3_of(const json::value* value)
	{
		if (!value || !value->is_array() || value->as_array().size() != 3) return std::nullopt;
		glm::vec3 out{};
		for (std::size_t i = 0; i < 3; ++i) {
			const json::value& item = value->as_array()[i];
			if (item.is_double()) out[static_cast<glm::length_t>(i)] = static_cast<float>(item.as_double());
			else if (item.is_int64()) out[static_cast<glm::length_t>(i)] = static_cast<float>(item.as_int64());
			else if (item.is_uint64()) out[static_cast<glm::length_t>(i)] = static_cast<float>(item.as_uint64());
			else return std::nullopt;
		}
		return out;
	}

	json::object ref_json(const std::string& world, std::span<const std::uint64_t> path)
	{
		json::array ids;
		ids.reserve(path.size());
		for (const std::uint64_t id : path) ids.emplace_back(json::string(desc::format_node_id(id)));
		return json::object{ { "context", json::string(world) }, { "path", std::move(ids) } };
	}

	// The node at `path` below the root of `world`, the root for an empty path.
	prefab_node* node_at(prefab_node& root, std::span<const std::uint64_t> path)
	{
		prefab_node* node = &root;
		for (const std::uint64_t id : path) {
			const auto it = std::ranges::find(node->children, id, &prefab_node::node_id);
			if (it == node->children.end()) return nullptr;
			node = &*it;
		}
		return node;
	}

	std::size_t index_arg(const json::object& args, std::size_t size)
	{
		const json::value* index = args.if_contains("index");
		const auto number = index ? cmd::json_schema::integer_of(*index) : std::nullopt;
		if (!number || *number < 0) return size;
		return std::min(static_cast<std::size_t>(*number), size);
	}

	struct context
	{
		edt::scene_command_sources sources;

		// The edit runs only when the caller may edit now; `apply` validates, calls `changing()` right before it
		// changes the worlds and fills the result. A commit follows a change.
		using edit_fn = std::function<cmd::result(edt::scene_document&, const json::object& args, json::object& out,
			const std::function<void()>& changing)>;

		cmd::result run(const cmd::call& call, std::string_view label, const edit_fn& apply)
		{
			const json::object& args = call.args.as_object();
			edt::scene_document& scene = sources.documents.scene();
			if (const json::value* document = args.if_contains("document")) {
				const auto id = cmd::json_schema::integer_of(*document);
				if (!id || static_cast<std::uint64_t>(*id) != scene.id()) {
					return failure(cmd::codes::not_found, "document is not the scene");
				}
			}
			if (call.from.kind == cmd::origin_kind::agent && sources.gesture_active && sources.gesture_active()) {
				return failure(cmd::codes::busy, "a person is in the middle of an edit; call again when it ends");
			}
			std::uint64_t expected = call.expected_generation;
			if (const json::value* given = args.if_contains("expected_generation")) {
				expected = static_cast<std::uint64_t>(cmd::json_schema::integer_of(*given).value_or(0));
			}
			if (expected != 0 && expected != scene.generation()) {
				cmd::result stale = failure(cmd::codes::stale,
					std::format("the scene is at generation {}, not {}: read it again", scene.generation(), expected));
				stale.value = json::object{ { "generation", scene.generation() } };
				return stale;
			}

			edt::edit_origin_scope origin(scene, call.from);
			bool changed = false;
			json::object out;
			cmd::result outcome = apply(scene, args, out, [&] {
				if (!changed) {
					// The panels point into the nodes that are about to move.
					sources.selection.clear();
					changed = true;
				}
			});
			if (!outcome.ok) return outcome;
			if (changed) scene.commit(label);
			out["generation"] = scene.generation();
			return cmd::result::success(std::move(out));
		}

		// A node of the edited desc, not of a prefab's content.
		static std::optional<cmd::result> own_node(const mutable_location& at)
		{
			if (!at.target.empty()) return failure(cmd::codes::failed, std::string(inside_instance));
			return std::nullopt;
		}

		bool locate(edt::scene_document& scene, const json::value& ref, mutable_location& at, cmd::result& error) const
		{
			if (!edt::scene_ref::locate(scene, ref, at, error)) return false;
			// A path past a plain node leads nowhere: only an instance has content of its own below it.
			if (!at.target.empty() && !edt::override_edit::referenced_prefab(*at.owner)) {
				error = failure(cmd::codes::not_found, "no node at that path");
				return false;
			}
			return true;
		}

		std::optional<cmd::result> check_type(const std::string& type) const
		{
			const desc::desc_system* descs = sources.services.descs();
			if (!descs || !descs->is_registered(type)) {
				return failure(cmd::codes::invalid_args, std::format("no desc type '{}'", type));
			}
			return std::nullopt;
		}

		// A component of `type` with `fields`, checked against the type.
		std::optional<cmd::result> make_component(const std::string& type, const json::object& fields,
			scn::prefab_desc::prefab_comp_node& out) const
		{
			if (auto bad = check_type(type)) return bad;
			if (!fields.empty()) {
				if (auto bad = check_fields(*sources.services.descs(), type, fields, fields)) return bad;
			}
			out.type_name = type;
			out.overrides = fields;
			return std::nullopt;
		}
	};

	// ---- scene.node.* -----------------------------------------------------------------------------------------

	cmd::result node_create(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Create node", [&ctx](edt::scene_document& scene, const json::object& args, json::object& out, const auto& changing) {
			mutable_location parent;
			cmd::result error;
			if (!ctx.locate(scene, args.at("parent"), parent, error)) return error;
			if (auto bad = context::own_node(parent)) return *bad;

			prefab_node node;
			node.node_id = desc::new_node_id();
			const json::value* name = args.if_contains("name");
			node.name = name ? std::string(name->as_string()) : std::string("Entity");
			if (const json::value* components = args.if_contains("components")) {
				for (const auto& [key, spec] : components->as_object()) {
					const json::object* object = spec.if_object();
					const json::value* type = object ? object->if_contains("type") : nullptr;
					const json::value* fields = object ? object->if_contains("fields") : nullptr;
					if (!type || !type->is_string() || (fields && !fields->is_object())) {
						return failure(cmd::codes::invalid_args, std::format("component '{}' is {{type: string, fields?: object}}", std::string_view(key)));
					}
					scn::prefab_desc::prefab_comp_node component;
					if (auto bad = ctx.make_component(std::string(type->as_string()),
							fields ? fields->as_object() : json::object{}, component)) {
						return *bad;
					}
					node.components[std::string(key)] = std::move(component);
				}
			}

			const std::uint64_t id = node.node_id;
			changing();
			auto& children = parent.owner->children;
			children.insert(children.begin() + static_cast<std::ptrdiff_t>(index_arg(args, children.size())), std::move(node));
			std::vector<std::uint64_t> path = parent.path;
			path.push_back(id);
			out["ref"] = ref_json(parent.world_name, path);
			return cmd::result::success();
		});
	}

	cmd::result node_delete(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Delete node", [&ctx](edt::scene_document& scene, const json::object& args, json::object&, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			if (auto bad = context::own_node(at)) return *bad;
			if (at.is_root) return failure(cmd::codes::invalid_args, "the root of a world cannot be deleted");

			prefab_node* parent = node_at(scene.world_descs[at.world].get_root(), std::span(at.path).first(at.path.size() - 1));
			ASSERT_MSG(parent, "the parent of a located node is gone");
			changing();
			std::erase_if(parent->children, [&](const prefab_node& n) { return n.node_id == at.path.back(); });
			return cmd::result::success();
		});
	}

	cmd::result node_duplicate(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Duplicate node", [&ctx](edt::scene_document& scene, const json::object& args, json::object& out, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			if (auto bad = context::own_node(at)) return *bad;
			if (at.is_root) return failure(cmd::codes::invalid_args, "the root of a world cannot be duplicated");

			prefab_node copy = *at.owner;
			scn::regenerate_node_ids(copy);
			copy.name += "_copy";
			const std::uint64_t id = copy.node_id;
			prefab_node* parent = node_at(scene.world_descs[at.world].get_root(), std::span(at.path).first(at.path.size() - 1));
			ASSERT_MSG(parent, "the parent of a located node is gone");
			changing();
			auto& children = parent->children;
			const auto original = std::ranges::find(children, at.path.back(), &prefab_node::node_id);
			children.insert(original + 1, std::move(copy));
			std::vector<std::uint64_t> path = at.path;
			path.back() = id;
			out["ref"] = ref_json(at.world_name, path);
			return cmd::result::success();
		});
	}

	cmd::result node_reparent(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Reparent node", [&ctx](edt::scene_document& scene, const json::object& args, json::object& out, const auto& changing) {
			mutable_location at;
			mutable_location parent;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error) || !ctx.locate(scene, args.at("parent"), parent, error)) return error;
			if (auto bad = context::own_node(at)) return *bad;
			if (auto bad = context::own_node(parent)) return *bad;
			if (at.is_root) return failure(cmd::codes::invalid_args, "the root of a world cannot be moved");
			if (at.world != parent.world) return failure(cmd::codes::invalid_args, "a node moves within its world only");
			if (parent.path.size() >= at.path.size() && std::equal(at.path.begin(), at.path.end(), parent.path.begin())) {
				return failure(cmd::codes::invalid_args, "a node cannot move under itself");
			}

			prefab_node& root = scene.world_descs[at.world].get_root();
			changing();
			prefab_node* old_parent = node_at(root, std::span(at.path).first(at.path.size() - 1));
			ASSERT_MSG(old_parent, "the parent of a located node is gone");
			const auto it = std::ranges::find(old_parent->children, at.path.back(), &prefab_node::node_id);
			prefab_node moved = std::move(*it);
			old_parent->children.erase(it);
			// Found again: taking the node out may have moved the new parent in memory.
			prefab_node* new_parent = node_at(root, parent.path);
			ASSERT_MSG(new_parent, "the new parent is gone");
			auto& children = new_parent->children;
			children.insert(children.begin() + static_cast<std::ptrdiff_t>(index_arg(args, children.size())), std::move(moved));
			std::vector<std::uint64_t> path = parent.path;
			path.push_back(at.path.back());
			out["ref"] = ref_json(at.world_name, path);
			return cmd::result::success();
		});
	}

	cmd::result node_rename(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Rename node", [&ctx](edt::scene_document& scene, const json::object& args, json::object&, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			if (auto bad = context::own_node(at)) return *bad;
			if (at.is_root) return failure(cmd::codes::invalid_args, "the root of a world is named by the level");

			std::string name(args.at("name").as_string());
			if (name == at.owner->name) return cmd::result::success();
			changing();
			at.owner->name = std::move(name);
			return cmd::result::success();
		});
	}

	// ---- component.* --------------------------------------------------------------------------------------------

	cmd::result component_add(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Add component", [&ctx](edt::scene_document& scene, const json::object& args, json::object&, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			if (auto bad = context::own_node(at)) return *bad;

			const std::string key(args.at("key").as_string());
			if (at.owner->components.contains(key)) {
				return failure(cmd::codes::invalid_args, std::format("the node already has a component '{}'", key));
			}
			const json::value* fields = args.if_contains("fields");
			scn::prefab_desc::prefab_comp_node component;
			if (auto bad = ctx.make_component(std::string(args.at("type").as_string()), fields ? fields->as_object() : json::object{}, component)) {
				return *bad;
			}
			changing();
			at.owner->components[key] = std::move(component);
			return cmd::result::success();
		});
	}

	cmd::result component_remove(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Remove component", [&ctx](edt::scene_document& scene, const json::object& args, json::object&, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			if (auto bad = context::own_node(at)) return *bad;

			const std::string key(args.at("key").as_string());
			if (!at.owner->components.contains(key)) {
				return failure(cmd::codes::not_found, std::format("the node has no component '{}'", key));
			}
			changing();
			at.owner->components.erase(key);
			return cmd::result::success();
		});
	}

	cmd::result component_set(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Edit component", [&ctx](edt::scene_document& scene, const json::object& args, json::object&, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			const std::string key(args.at("key").as_string());
			const json::object& patch = args.at("patch").as_object();
			const desc::desc_system* descs = ctx.sources.services.descs();
			if (!descs) return failure(cmd::codes::failed, "no desc system");

			if (at.target.empty()) {
				const auto component = at.owner->components.find(key);
				if (component == at.owner->components.end()) {
					return failure(cmd::codes::not_found, std::format("the node has no component '{}'", key));
				}
				json::object edited = merge_patch(component->second.overrides, patch).as_object();
				if (auto bad = check_fields(*descs, component->second.type_name, patch, edited)) return *bad;
				if (edited == component->second.overrides) return cmd::result::success();
				changing();
				component->second.overrides = std::move(edited);
				return cmd::result::success();
			}

			// A node of the prefab's content: the edit becomes override records on the instance.
			scn::ecs_assembler* assembler = ctx.sources.services.assembler();
			if (!assembler) return failure(cmd::codes::failed, "no assembler to expand the prefab instance");
			const auto base = edt::override_edit::find_base_node(*assembler, *at.owner, at.target);
			if (!base) return failure(cmd::codes::not_found, "no node at that path inside the instance");
			const auto component = base->components.find(key);
			if (component == base->components.end()) {
				return failure(cmd::codes::not_found, std::format("the node has no component '{}'", key));
			}
			const json::object base_json = edt::override_edit::component_json(component->second);
			const json::object effective = edt::override_edit::effective_component(*at.owner, at.target, key, base_json);
			json::object edited = merge_patch(effective, patch).as_object();
			if (auto bad = check_fields(*descs, component->second.type_name, patch, edited)) return *bad;
			if (edited == effective) return cmd::result::success();
			changing();
			edt::override_edit::write_component(*at.owner, at.target, key, base_json, edited);
			return cmd::result::success();
		});
	}

	// ---- transform.set ----------------------------------------------------------------------------------------

	cmd::result transform_set(context& ctx, const cmd::call& call)
	{
		return ctx.run(call, "Move node", [&ctx](edt::scene_document& scene, const json::object& args, json::object&, const auto& changing) {
			mutable_location at;
			cmd::result error;
			if (!ctx.locate(scene, args.at("ref"), at, error)) return error;
			const auto position = vec3_of(args.if_contains("position"));
			const auto rotation = vec3_of(args.if_contains("rotation"));
			const auto scale = vec3_of(args.if_contains("scale"));
			for (const char* name : { "position", "rotation", "scale" }) {
				if (args.contains(name) && !vec3_of(args.if_contains(name))) {
					return failure(cmd::codes::invalid_args, std::format("{} is [x, y, z]", name));
				}
			}
			if (!position && !rotation && !scale) return failure(cmd::codes::invalid_args, "nothing to set: give position, rotation or scale");

			if (at.target.empty()) {
				prefab_node& node = *at.owner;
				changing();
				if (position) node.position = *position;
				if (rotation) node.rotation = *rotation;
				if (scale) node.scale = *scale;
				node.has_transform = true;
				return cmd::result::success();
			}

			scn::ecs_assembler* assembler = ctx.sources.services.assembler();
			if (!assembler) return failure(cmd::codes::failed, "no assembler to expand the prefab instance");
			const auto base = edt::override_edit::find_base_node(*assembler, *at.owner, at.target);
			if (!base) return failure(cmd::codes::not_found, "no node at that path inside the instance");
			glm::vec3 pos{};
			glm::vec3 rot{};
			glm::vec3 scl{ 1.0f };
			edt::override_edit::effective_transform(*at.owner, at.target, *base, pos, rot, scl);
			changing();
			edt::override_edit::write_transform(*at.owner, at.target, *base,
				position.value_or(pos), rotation.value_or(rot), scale.value_or(scl));
			return cmd::result::success();
		});
	}

	// ---- registration -----------------------------------------------------------------------------------------

	constexpr std::string_view common_args = R"("document":{"type":"integer","minimum":1,"description":"the scene document's id; default: the scene"},
		"expected_generation":{"type":"integer","minimum":0,"description":"the generation the edit was built on (from scene.query or an earlier edit); another one gives stale"})";

	std::string args_with(std::string_view properties, std::string_view required)
	{
		return std::format(R"({{"type":"object","properties":{{{},{}}},"required":[{}],"additionalProperties":false}})",
			properties, common_args, required);
	}

	constexpr std::string_view generation_result = R"({"type":"object","properties":{"generation":{"type":"integer"}},"required":["generation"]})";
	constexpr std::string_view ref_result = R"({"type":"object","properties":{"generation":{"type":"integer"},"ref":@REF@},"required":["generation"]})";
}

std::vector<edt::registration> edt::register_scene_commands(cmd::command_registry& commands, scene_command_sources sources)
{
	auto ctx = std::make_shared<context>(context{ std::move(sources) });
	std::vector<registration> out;
	const auto add = [&](std::string id, std::string summary, std::string args, std::string_view result,
		cmd::result (*body)(context&, const cmd::call&)) {
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = parse_schema(args);
		desc.result_schema = parse_schema(result);
		desc.mutates = true;
		desc.invoke = [ctx, body](const cmd::call& call) { return body(*ctx, call); };
		registration added = commands.add(std::move(desc));
		if (added.active()) out.push_back(std::move(added));
	};

	add("scene.node.create", "Create a node under `parent` (a ref; the world root: path []), with components {key: {type, fields?}}",
		args_with(R"("parent":@REF@,"name":{"type":"string","minLength":1},"index":{"type":"integer","minimum":0},
			"components":{"type":"object","description":"{key: {type: desc type, fields?: object}}"})",
			R"("parent")"),
		ref_result, &node_create);
	add("scene.node.delete", "Delete a node with everything under it", args_with(R"("ref":@REF@)", R"("ref")"), generation_result, &node_delete);
	add("scene.node.duplicate", "Copy a node with everything under it right after it; the copy gets new ids",
		args_with(R"("ref":@REF@)", R"("ref")"), ref_result, &node_duplicate);
	add("scene.node.reparent", "Move a node under `parent` (same world) at `index` (default: last)",
		args_with(R"("ref":@REF@,"parent":@REF@,"index":{"type":"integer","minimum":0})", R"("ref","parent")"), ref_result, &node_reparent);
	add("scene.node.rename", "Rename a node (names are for display: nodes are addressed by id)",
		args_with(R"("ref":@REF@,"name":{"type":"string","minLength":1})", R"("ref","name")"), generation_result, &node_rename);
	add("component.add", "Add a component of desc `type` under `key`, with `fields` checked against the type's schema",
		args_with(R"("ref":@REF@,"key":{"type":"string","minLength":1},"type":{"type":"string","minLength":1},"fields":{"type":"object"})",
			R"("ref","key","type")"),
		generation_result, &component_add);
	add("component.remove", "Remove the component `key` of a node", args_with(R"("ref":@REF@,"key":{"type":"string","minLength":1})", R"("ref","key")"),
		generation_result, &component_remove);
	add("component.set", "Change fields of the component `key` with a JSON Merge Patch (RFC 7386; null clears a field); inside a prefab instance the change becomes override records",
		args_with(R"("ref":@REF@,"key":{"type":"string","minLength":1},"patch":{"type":"object"})", R"("ref","key","patch")"),
		generation_result, &component_set);
	add("transform.set", "Set position, rotation (Euler degrees) and/or scale of a node; inside a prefab instance as override records",
		args_with(R"("ref":@REF@,"position":{"type":"array","items":{"type":"number"}},"rotation":{"type":"array","items":{"type":"number"}},"scale":{"type":"array","items":{"type":"number"}})",
			R"("ref")"),
		generation_result, &transform_set);
	return out;
}
