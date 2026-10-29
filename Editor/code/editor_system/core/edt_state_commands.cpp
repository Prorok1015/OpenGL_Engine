#include <snk/editor/edt_state_commands.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_schema.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_system.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include "edt_asset_layers.h"
#include "edt_override_edit.h"
#include "edt_scene_document.h"
#include "edt_scene_ref.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;

namespace {
	using prefab_node = scn::prefab_desc::prefab_node;

	constexpr std::size_t max_children_listed = 200;
	constexpr std::size_t max_overrides_listed = 200;
	// An asset scan stops after this many directory entries; the result says so.
	constexpr std::size_t max_asset_scan = 100000;
	constexpr int max_asset_depth = 24;
	constexpr std::string_view prefab_type = "prefab_desc";

	std::string text_of(const json::string& text)
	{
		return std::string(text.data(), text.size());
	}

	bool valid_utf8(std::string_view text);
	bool safe_text(std::string_view text);

	// Text that goes into the answer: bytes that are not valid UTF-8 would make the JSON of the whole reply invalid
	// (an MCP client drops it), so such a string is sent with its non-ASCII bytes replaced.
	json::string safe_string(std::string_view text)
	{
		if (valid_utf8(text)) return json::string(text);
		std::string cleaned(text);
		for (char& c : cleaned) {
			if (static_cast<unsigned char>(c) >= 0x80) c = '?';
		}
		return json::string(cleaned);
	}

	constexpr std::size_t max_glob_length = 128;
	constexpr std::size_t max_glob_stars = 16;
	constexpr std::size_t max_cursor_length = 512;

	// A pattern costs text x pattern per tested name on the main thread, so its size and its stars are limited. The
	// runs of stars are collapsed (a*****b is a*b). Returns why the pattern is refused, empty if it is fine.
	std::string check_glob(std::string& glob)
	{
		if (glob.size() > max_glob_length) return std::format("a glob is limited to {} characters", max_glob_length);
		if (!safe_text(glob)) return "a glob takes no control characters and no backslashes";
		std::string collapsed;
		collapsed.reserve(glob.size());
		std::size_t stars = 0;
		for (const char c : glob) {
			if (c == '*') {
				if (!collapsed.empty() && collapsed.back() == '*') continue;
				++stars;
			}
			collapsed += c;
		}
		if (stars > max_glob_stars) return std::format("a glob takes at most {} * wildcards", max_glob_stars);
		glob = std::move(collapsed);
		return {};
	}

	bool cursor_ok(std::string_view cursor)
	{
		return cursor.size() <= max_cursor_length && safe_text(cursor);
	}

	json::value parse_schema(std::string_view text, std::string_view what)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "the schema of {} is not JSON: {}", what, ec.message());
		return parsed;
	}

	std::string with_ref(std::string text)
	{
		return edt::scene_ref::with_ref(std::move(text));
	}

	void keep(std::vector<edt::registration>& out, edt::registration added)
	{
		if (added.active()) out.push_back(std::move(added));
	}

	std::int64_t int_arg(const json::object& args, const char* key, std::int64_t fallback)
	{
		if (const json::value* value = args.if_contains(key)) {
			if (const auto number = cmd::json_schema::integer_of(*value)) return *number;
		}
		return fallback;
	}

	std::int64_t page_size_of(const json::object& args)
	{
		return std::clamp<std::int64_t>(int_arg(args, "page_size", edt::state_query_default_page_size), 1, edt::state_query_max_page_size);
	}

	std::optional<std::uint64_t> parse_hex(std::string_view text)
	{
		if (text.empty()) return std::nullopt;
		std::uint64_t value = 0;
		const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
		return value;
	}

	std::vector<std::string_view> split(std::string_view text, char separator)
	{
		std::vector<std::string_view> parts;
		std::size_t begin = 0;
		for (;;) {
			const std::size_t end = text.find(separator, begin);
			if (end == std::string_view::npos) {
				parts.push_back(text.substr(begin));
				return parts;
			}
			parts.push_back(text.substr(begin, end - begin));
			begin = end + 1;
		}
	}

	char lower(char c)
	{
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	// * and ? wildcards, case-insensitive; a pattern without any is a substring test.
	bool glob_match(std::string_view pattern, std::string_view text)
	{
		std::string wrapped;
		if (pattern.find_first_of("*?") == std::string_view::npos) {
			wrapped = "*" + std::string(pattern) + "*";
			pattern = wrapped;
		}
		std::size_t p = 0;
		std::size_t t = 0;
		std::size_t star = std::string_view::npos;
		std::size_t mark = 0;
		while (t < text.size()) {
			if (p < pattern.size() && pattern[p] == '*') {
				star = p++;
				mark = t;
			} else if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
				++p;
				++t;
			} else if (star != std::string_view::npos) {
				p = star + 1;
				t = ++mark;
			} else {
				return false;
			}
		}
		while (p < pattern.size() && pattern[p] == '*') ++p;
		return p == pattern.size();
	}

	std::uint64_t hash_text(std::string_view text)
	{
		return desc::schema_id(text);
	}

	// ---- refs --------------------------------------------------------------------------------------------------

	json::array path_json(std::span<const std::uint64_t> path)
	{
		json::array out;
		out.reserve(path.size());
		for (const std::uint64_t id : path) out.emplace_back(json::string(desc::format_node_id(id)));
		return out;
	}

	json::object ref_json(const std::string& world, std::span<const std::uint64_t> path)
	{
		json::object out;
		out["context"] = safe_string(world);
		out["path"] = path_json(path);
		return out;
	}

	json::object ref_json(const std::string& world, std::span<const std::uint64_t> path, std::uint64_t child)
	{
		std::vector<std::uint64_t> full(path.begin(), path.end());
		full.push_back(child);
		return ref_json(world, full);
	}

	json::value vec3_json(const glm::vec3& v)
	{
		return json::array{ static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z) };
	}

	json::array sorted_keys(const std::unordered_map<std::string, scn::prefab_desc::prefab_comp_node>& components)
	{
		std::vector<std::string> keys;
		keys.reserve(components.size());
		for (const auto& entry : components) keys.push_back(entry.first);
		std::ranges::sort(keys);
		json::array out;
		out.reserve(keys.size());
		for (const auto& key : keys) out.emplace_back(safe_string(key));
		return out;
	}

	// The tag of the prefab a node instantiates, empty if it is a plain node.
	std::string prefab_of(const prefab_node& node)
	{
		for (const auto& entry : node.components) {
			if (entry.second.type_name == prefab_type && entry.second.parent_desc.is_valid()) {
				return entry.second.parent_desc.get_tag().string();
			}
		}
		return {};
	}

	bool references_prefab(const prefab_node& node)
	{
		return std::ranges::any_of(node.components, [](const auto& entry) { return entry.second.type_name == prefab_type; });
	}

	struct world_lookup
	{
		const scn::world_desc* desc = nullptr;
		std::string name;
	};

	// A world by name; a "<level>/<world>" context is read as its world.
	world_lookup find_world(const edt::scene_document& scene, std::string_view context)
	{
		const auto index = edt::scene_ref::find_world(scene, context);
		if (!index) return {};
		return { &scene.world_descs[*index], scene.world_names[*index] };
	}

	struct located
	{
		world_lookup world;
		// The deepest node of the world desc on the path; the world root for an empty path.
		const prefab_node* owner = nullptr;
		std::vector<std::uint64_t> path;
		// The ids below `owner`, inside the prefab it instantiates.
		std::vector<std::uint64_t> target;
		bool is_root = false;
	};

	// False with `error` filled when the ref is malformed or leads nowhere.
	bool locate(const edt::scene_document& scene, const json::value& ref, located& out, cmd::result& error)
	{
		edt::scene_ref::location<const prefab_node> at;
		if (!edt::scene_ref::locate(scene, ref, at, error)) return false;
		out.world = { &scene.world_descs[at.world], std::move(at.world_name) };
		out.owner = at.owner;
		out.path = std::move(at.path);
		out.target = std::move(at.target);
		out.is_root = at.is_root;
		return true;
	}

	bool walk(const std::vector<prefab_node>& nodes, std::vector<std::uint64_t>& path,
		const std::function<bool(const prefab_node&, const std::vector<std::uint64_t>&)>& visit)
	{
		for (const auto& node : nodes) {
			path.push_back(node.node_id);
			const bool go = visit(node, path) && walk(node.children, path, visit);
			path.pop_back();
			if (!go) return false;
		}
		return true;
	}

	const edt::scene_document* scene_for(edt::document_manager& documents, const json::object& args, cmd::result& error)
	{
		const edt::scene_document& scene = documents.scene();
		const json::value* requested = args.if_contains("document");
		if (!requested) return &scene;
		const auto id = cmd::json_schema::integer_of(*requested);
		if (!id || static_cast<std::uint64_t>(*id) != scene.id()) {
			error = cmd::result::failure(cmd::codes::not_found, std::format("no scene document {}", id ? *id : 0));
			return nullptr;
		}
		return &scene;
	}

	// ---- scene.query -------------------------------------------------------------------------------------------

	struct query_cursor
	{
		std::uint64_t document = 0;
		std::uint64_t generation = 0;
		std::uint64_t filter = 0;
		std::uint64_t offset = 0;
	};

	std::string encode_cursor(const query_cursor& cursor)
	{
		return std::format("{:x}.{:x}.{:x}.{:x}", cursor.document, cursor.generation, cursor.filter, cursor.offset);
	}

	std::optional<query_cursor> decode_cursor(std::string_view text)
	{
		const auto parts = split(text, '.');
		if (parts.size() != 4) return std::nullopt;
		const auto document = parse_hex(parts[0]);
		const auto generation = parse_hex(parts[1]);
		const auto filter = parse_hex(parts[2]);
		const auto offset = parse_hex(parts[3]);
		if (!document || !generation || !filter || !offset) return std::nullopt;
		return query_cursor{ *document, *generation, *filter, *offset };
	}

	cmd::result scene_query(const edt::state_command_sources& sources, const cmd::call& request)
	{
		const json::object& args = request.args.as_object();
		cmd::result error;
		const edt::scene_document* scene = scene_for(sources.documents, args, error);
		if (!scene) return error;

		std::string world_name;
		if (const json::value* world = args.if_contains("world")) {
			world_name = text_of(world->as_string());
		} else if (const std::string* active = scene->active_world_name()) {
			world_name = *active;
		} else {
			return cmd::result::failure(cmd::codes::not_found, "the document has no worlds");
		}
		const world_lookup world = find_world(*scene, world_name);
		if (!world.desc) return cmd::result::failure(cmd::codes::not_found, std::format("no world '{}' in the document", world_name));

		std::string name_glob;
		std::vector<std::string> wanted;
		std::optional<located> under;
		if (const json::value* filter_value = args.if_contains("filter")) {
			const json::object& filter = filter_value->as_object();
			if (const json::value* glob = filter.if_contains("name_glob")) name_glob = text_of(glob->as_string());
			if (const json::value* components = filter.if_contains("has_component")) {
				for (const json::value& item : components->as_array()) wanted.push_back(text_of(item.as_string()));
			}
			if (const json::value* ref = filter.if_contains("under")) {
				located at;
				if (!locate(*scene, *ref, at, error)) return error;
				if (at.world.name != world.name) {
					return cmd::result::failure(cmd::codes::invalid_args, std::format("under is a node of world '{}', but the query is for world '{}'; pass world", at.world.name, world.name));
				}
				if (!at.target.empty()) {
					return cmd::result::failure(cmd::codes::invalid_args, "under is inside a prefab instance; scene.query lists the nodes of the world desc, use node.get for the content of an instance");
				}
				under = std::move(at);
			}
		}
		if (!name_glob.empty()) {
			if (const std::string why = check_glob(name_glob); !why.empty()) return cmd::result::failure(cmd::codes::invalid_args, why);
		}
		std::ranges::sort(wanted);

		// What the cursor is bound to besides the generation: the same query must come back with it.
		std::string identity = world.name + '\n' + name_glob + '\n';
		for (const auto& component : wanted) identity += component + ',';
		identity += '\n';
		if (under) for (const std::uint64_t id : under->path) identity += desc::format_node_id(id);
		query_cursor expected;
		expected.document = scene->id();
		expected.generation = scene->generation();
		expected.filter = hash_text(identity);

		std::uint64_t offset = 0;
		if (const json::value* text = args.if_contains("cursor")) {
			if (!cursor_ok(std::string_view(text->as_string()))) {
				return cmd::result::failure(cmd::codes::invalid_args, "the cursor is not one this command returned");
			}
			const auto cursor = decode_cursor(std::string_view(text->as_string()));
			if (!cursor) return cmd::result::failure(cmd::codes::invalid_args, "the cursor is not one this command returned");
			if (cursor->document != expected.document || cursor->filter != expected.filter) {
				return cmd::result::failure(cmd::codes::invalid_args, "the cursor belongs to another document or another query; start again without it");
			}
			if (cursor->generation != expected.generation) {
				return cmd::result::failure(cmd::codes::stale, "the document changed since this cursor was issued; query again from the start");
			}
			offset = cursor->offset;
		}

		const std::int64_t page_size = page_size_of(args);
		const prefab_node& root = world.desc->get_root();
		const std::vector<prefab_node>& start = under && !under->is_root ? under->owner->children : root.children;
		std::vector<std::uint64_t> path = under ? under->path : std::vector<std::uint64_t>{};

		json::array items;
		std::uint64_t seen = 0;
		bool more = false;
		walk(start, path, [&](const prefab_node& node, const std::vector<std::uint64_t>& node_path) {
			if (!name_glob.empty() && !glob_match(name_glob, node.name)) return true;
			const bool has_all = std::ranges::all_of(wanted, [&node](const std::string& component) {
				if (node.components.contains(component)) return true;
				return std::ranges::any_of(node.components, [&component](const auto& entry) { return entry.second.type_name == component; });
			});
			if (!has_all) return true;
			if (seen++ < offset) return true;
			if (static_cast<std::int64_t>(items.size()) >= page_size) {
				more = true;
				return false;
			}
			json::object item;
			item["ref"] = ref_json(world.name, node_path);
			item["name"] = safe_string(node.name);
			item["components"] = sorted_keys(node.components);
			if (const std::string prefab = prefab_of(node); !prefab.empty()) item["prefab"] = safe_string(prefab);
			items.push_back(std::move(item));
			return true;
		});

		json::object out;
		out["document"] = scene->id();
		out["world"] = safe_string(world.name);
		out["generation"] = scene->generation();
		out["items"] = std::move(items);
		if (more) {
			expected.offset = offset + static_cast<std::uint64_t>(page_size);
			out["next_cursor"] = json::string(encode_cursor(expected));
		}
		return cmd::result::success(std::move(out));
	}

	// ---- node.get ----------------------------------------------------------------------------------------------

	json::object transform_json(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale)
	{
		json::object out;
		out["position"] = vec3_json(position);
		out["rotation"] = vec3_json(rotation);
		out["scale"] = vec3_json(scale);
		return out;
	}

	cmd::result node_get(const edt::state_command_sources& sources, const cmd::call& request)
	{
		const json::object& args = request.args.as_object();
		const edt::scene_document& scene = sources.documents.scene();
		located at;
		cmd::result error;
		if (!locate(scene, args.at("ref"), at, error)) return error;

		std::set<std::string> only;
		if (const json::value* components = args.if_contains("components")) {
			for (const json::value& item : components->as_array()) only.insert(text_of(item.as_string()));
		}
		const auto wanted = [&only](const std::string& key, const std::string& type) {
			return only.empty() || only.contains(key) || only.contains(type);
		};

		// The node as the document defines it, or its base inside a prefab instance with the owner's records on top.
		std::optional<prefab_node> base;
		const prefab_node* subject = at.owner;
		if (!at.target.empty()) {
			base = edt::override_edit::find_base_node(*sources.services.assembler(), *at.owner, at.target);
			if (!base) return cmd::result::failure(cmd::codes::not_found, "no node at that path inside the prefab instance");
			subject = &*base;
		}

		std::map<std::string, const scn::prefab_desc::prefab_comp_node*> sorted;
		for (const auto& entry : subject->components) {
			if (entry.second.type_name == prefab_type) continue;
			if (wanted(entry.first, entry.second.type_name)) sorted.emplace(entry.first, &entry.second);
		}
		json::object components;
		for (const auto& [key, comp] : sorted) {
			json::object body = edt::override_edit::component_json(*comp);
			if (!at.target.empty()) body = edt::override_edit::effective_component(*at.owner, at.target, key, std::move(body));
			if (!body.contains("__type")) body["__type"] = safe_string(comp->type_name);
			components[key] = std::move(body);
		}

		json::object out;
		out["ref"] = ref_json(at.world.name, at.path);
		out["document"] = scene.id();
		out["generation"] = scene.generation();
		out["world"] = safe_string(at.world.name);
		out["name"] = safe_string(at.is_root && subject->name.empty() ? std::string(at.world.desc->get_name()) : subject->name);
		if (at.path.empty()) {
			out["parent"] = nullptr;
		} else {
			out["parent"] = ref_json(at.world.name, std::span<const std::uint64_t>(at.path).first(at.path.size() - 1));
		}
		out["in_instance"] = !at.target.empty();
		if (at.target.empty()) {
			if (const std::string prefab = prefab_of(*at.owner); !prefab.empty()) out["prefab"] = safe_string(prefab);
			if (at.owner->has_transform) out["transform"] = transform_json(at.owner->position, at.owner->rotation, at.owner->scale);
		} else {
			glm::vec3 position = base->position;
			glm::vec3 rotation = base->rotation;
			glm::vec3 scale = base->scale;
			edt::override_edit::effective_transform(*at.owner, at.target, *base, position, rotation, scale);
			out["transform"] = transform_json(position, rotation, scale);
		}
		out["components"] = std::move(components);

		struct child_row
		{
			std::uint64_t id = 0;
			std::string name;
			json::array components;
			bool has_children = false;
		};
		std::vector<child_row> rows;
		std::size_t child_total = 0;
		const bool composed = !at.target.empty() || !at.owner->overrides.empty() || references_prefab(*at.owner);
		// The cap applies before any child is expanded: expanding costs a copy of the prefab content of each one.
		if (composed) {
			for (const auto& listed : edt::override_edit::list_children(*sources.services.assembler(), *at.owner, at.target, max_children_listed, &child_total)) {
				rows.push_back({ listed.node.node_id, listed.node.name, sorted_keys(listed.node.components), listed.has_children });
			}
		} else {
			child_total = at.owner->children.size();
			for (const auto& child : at.owner->children) {
				if (rows.size() >= max_children_listed) break;
				rows.push_back({ child.node_id, child.name, sorted_keys(child.components), !child.children.empty() });
			}
		}
		json::array children;
		for (auto& row_data : rows) {
			json::object row;
			row["ref"] = ref_json(at.world.name, at.path, row_data.id);
			row["name"] = safe_string(row_data.name);
			row["components"] = std::move(row_data.components);
			row["has_children"] = row_data.has_children;
			children.push_back(std::move(row));
		}
		out["child_count"] = static_cast<std::uint64_t>(child_total);
		out["children_truncated"] = child_total > rows.size();
		out["children"] = std::move(children);

		// The records the owner holds: all of them for the node itself, those aimed at this node inside an instance.
		json::array overrides;
		std::size_t matching = 0;
		for (const auto& record : at.owner->overrides) {
			if (!at.target.empty() && record.target != at.target) continue;
			if (++matching > max_overrides_listed) break;
			overrides.push_back(scn::override_record_to_json(record));
		}
		out["overrides"] = std::move(overrides);
		return cmd::result::success(std::move(out));
	}

	// ---- component.schema --------------------------------------------------------------------------------------

	cmd::result component_schema(const edt::state_command_sources& sources, const cmd::call& request)
	{
		const std::string type = text_of(request.args.as_object().at("type").as_string());
		const desc::desc_system& descs = *sources.services.descs();
		const desc::type_schema* schema = descs.find_schema(type);
		if (!schema) {
			if (descs.is_registered(type)) {
				return cmd::result::failure(cmd::codes::not_found, std::format("'{}' has no schema yet (it is not on the generic desc schema)", type));
			}
			return cmd::result::failure(cmd::codes::not_found, std::format("no desc type '{}'; the __type of a component in node.get names one", type));
		}
		json::object out;
		out["type"] = json::string(schema->name);
		out["version"] = schema->version;
		out["kind"] = json::string(desc::to_string(schema->kind));
		out["is_component"] = schema->is_component;
		out["schema"] = desc::to_json_schema(*schema, descs.schemas());
		return cmd::result::success(std::move(out));
	}

	// ---- asset.query -------------------------------------------------------------------------------------------

	struct asset_row
	{
		std::string path; // res-relative, '/' separated
		fs::path physical;
		std::size_t layer = 0;
	};

	struct scan_state
	{
		std::size_t scanned = 0;
		std::size_t bad_names = 0;
		bool truncated = false;
	};

	bool valid_utf8(std::string_view text)
	{
		std::size_t i = 0;
		while (i < text.size()) {
			const auto c = static_cast<unsigned char>(text[i]);
			if (c < 0x80) {
				++i;
				continue;
			}
			std::size_t extra = 0;
			std::uint32_t code = 0;
			if (c >= 0xC2 && c <= 0xDF) {
				extra = 1;
				code = c & 0x1Fu;
			} else if (c >= 0xE0 && c <= 0xEF) {
				extra = 2;
				code = c & 0x0Fu;
			} else if (c >= 0xF0 && c <= 0xF4) {
				extra = 3;
				code = c & 0x07u;
			} else {
				return false;
			}
			if (i + extra >= text.size()) return false;
			for (std::size_t k = 1; k <= extra; ++k) {
				const auto next = static_cast<unsigned char>(text[i + k]);
				if ((next & 0xC0) != 0x80) return false;
				code = (code << 6) | (next & 0x3Fu);
			}
			if (extra == 2 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) return false;
			if (extra == 3 && (code < 0x10000 || code > 0x10FFFF)) return false;
			i += extra + 1;
		}
		return true;
	}

	// Control characters (a NUL ends a path at the OS call) and backslashes (a separator on Windows) never belong in a
	// path or a pattern the agent sends.
	bool safe_text(std::string_view text)
	{
		return std::ranges::none_of(text, [](char c) {
			const auto u = static_cast<unsigned char>(c);
			return u < 0x20 || u == 0x7f || c == '\\';
		});
	}

	// `candidate` lies inside `root` once both are normalised.
	bool is_under(const fs::path& root, const fs::path& candidate)
	{
		const fs::path relative = candidate.lexically_normal().lexically_relative(root.lexically_normal());
		return !relative.empty() && *relative.begin() != "..";
	}

	// Every component of root/relative is a real directory, not a link.
	bool plain_directory(const fs::path& root, const fs::path& relative)
	{
		fs::path current = root;
		for (const auto& part : relative) {
			if (part.empty()) continue;
			current /= part;
			std::error_code ec;
			const auto status = fs::symlink_status(current, ec);
			if (ec || status.type() != fs::file_type::directory) return false;
		}
		return true;
	}

	void collect_assets(const std::vector<fs::path>& layers, const fs::path& directory, int depth,
		std::vector<asset_row>& out, scan_state& state)
	{
		if (depth > max_asset_depth) {
			state.truncated = true;
			return;
		}
#ifdef _WIN32
		constexpr bool case_insensitive = true;
#else
		constexpr bool case_insensitive = false;
#endif
		struct found
		{
			std::string name;
			fs::path path;
			bool is_dir = false;
			std::size_t layer = 0;
		};
		// An entry of an upper layer shadows the same name below, whatever its kind.
		std::map<std::string, found> merged;
		for (std::size_t i = 0; i < layers.size() && !state.truncated; ++i) {
			const fs::path dir = layers[i] / directory;
			if (!is_under(layers[i], dir) || !plain_directory(layers[i], directory)) continue;
			std::error_code ec;
			for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator{}; it.increment(ec)) {
				if (state.scanned >= max_asset_scan) {
					state.truncated = true;
					break;
				}
				++state.scanned;
				std::error_code status_ec;
				const fs::file_status status = it->symlink_status(status_ec);
				if (status_ec) continue;
				const bool is_dir = status.type() == fs::file_type::directory;
				// Links, devices and sockets are not assets (and a link could lead out of the layer).
				if (!is_dir && status.type() != fs::file_type::regular) continue;
				std::string name = it->path().filename().string();
				if (!valid_utf8(name)) {
					++state.bad_names;
					continue;
				}
				const std::string key = edt::layer_merge_key(name, case_insensitive);
				merged[key] = found{ std::move(name), it->path(), is_dir, i };
			}
		}
		for (const auto& [key, entry] : merged) {
			const fs::path relative = directory / entry.name;
			if (entry.is_dir) {
				if (!state.truncated) collect_assets(layers, relative, depth + 1, out, state);
			} else {
				out.push_back({ relative.generic_string(), entry.path, entry.layer });
			}
		}
	}

	std::string hex_encode(std::string_view text)
	{
		std::string out;
		out.reserve(text.size() * 2);
		for (const char c : text) out += std::format("{:02x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
		return out;
	}

	std::optional<std::string> hex_decode(std::string_view text)
	{
		if (text.size() % 2 != 0) return std::nullopt;
		std::string out;
		out.reserve(text.size() / 2);
		for (std::size_t i = 0; i < text.size(); i += 2) {
			const auto byte = parse_hex(text.substr(i, 2));
			if (!byte) return std::nullopt;
			out += static_cast<char>(*byte);
		}
		return out;
	}

	std::string kind_of_asset(const edt::asset_kind_registry& kinds, const std::string& path)
	{
		if (const auto* kind = kinds.kind_for_open(path)) return kind->id;
		if (const auto* kind = kinds.kind_for_drop(path)) return kind->id;
		return {};
	}

	cmd::result asset_query(const edt::state_command_sources& sources, const cmd::call& request)
	{
		const json::object& args = request.args.as_object();
		std::string glob;
		std::string kind;
		std::string directory;
		if (const json::value* v = args.if_contains("glob")) glob = text_of(v->as_string());
		if (const json::value* v = args.if_contains("kind")) kind = text_of(v->as_string());
		if (const json::value* v = args.if_contains("directory")) directory = text_of(v->as_string());

		// The schema subset has no maxLength or pattern, so the limits are enforced here.
		constexpr std::size_t max_text = 512;
		if (!glob.empty()) {
			if (const std::string why = check_glob(glob); !why.empty()) return cmd::result::failure(cmd::codes::invalid_args, why);
		}
		if (glob.size() > max_text || kind.size() > max_text || directory.size() > max_text) {
			return cmd::result::failure(cmd::codes::invalid_args, "glob, kind and directory are limited to 512 characters");
		}
		if (!safe_text(glob) || !safe_text(kind) || !safe_text(directory)) {
			return cmd::result::failure(cmd::codes::invalid_args, "glob, kind and directory take no control characters and no backslashes");
		}
		if (directory.starts_with("res://")) directory.erase(0, 6);
		fs::path relative = fs::path(directory).lexically_normal();
		bool plain = directory.find(':') == std::string::npos && !relative.is_absolute() && !relative.has_root_name() && !relative.has_root_directory();
		for (const auto& part : relative) {
			const std::string text = part.string();
			// Windows drops a trailing dot or space from a name, so "..." or ".. " would reach the parent there.
			if (text == ".." || (text != "." && !text.empty() && (text.back() == '.' || text.back() == ' '))) plain = false;
		}
		if (!plain) {
			return cmd::result::failure(cmd::codes::invalid_args, "directory is a path under res:// (no leading slash, no .., no drive or stream colon)");
		}
		if (relative == ".") relative.clear();

		const std::uint64_t filter = hash_text(glob + '\n' + kind + '\n' + relative.generic_string());
		std::optional<std::string> after;
		if (const json::value* text = args.if_contains("cursor")) {
			if (!cursor_ok(std::string_view(text->as_string()))) {
				return cmd::result::failure(cmd::codes::invalid_args, "the cursor is not one this command returned");
			}
			const auto parts = split(std::string_view(text->as_string()), '.');
			const auto cursor_filter = parts.size() == 2 ? parse_hex(parts[0]) : std::nullopt;
			after = parts.size() == 2 ? hex_decode(parts[1]) : std::nullopt;
			if (!cursor_filter || !after) return cmd::result::failure(cmd::codes::invalid_args, "the cursor is not one this command returned");
			if (*cursor_filter != filter) {
				return cmd::result::failure(cmd::codes::invalid_args, "the cursor belongs to another query; start again without it");
			}
		}

		const std::vector<fs::path> layers = sources.asset_layers ? sources.asset_layers() : std::vector<fs::path>{};
		std::vector<asset_row> rows;
		scan_state state;
		if (!layers.empty()) collect_assets(layers, relative, 0, rows, state);
		if (state.bad_names > 0) {
			egLOG_WARN("editor", "asset.query skipped {} names that are not valid UTF-8", state.bad_names);
		}

		// Keyset paging over the sorted paths: an asset added or removed between two pages moves no other row.
		std::ranges::sort(rows, {}, &asset_row::path);
		const std::int64_t page_size = page_size_of(args);
		json::array items;
		bool more = false;
		std::string last;
		const auto first = after ? std::ranges::upper_bound(rows, *after, {}, &asset_row::path) : rows.begin();
		for (auto it = first; it != rows.end(); ++it) {
			if (!glob.empty() && !glob_match(glob, it->path)) continue;
			const std::string row_kind = kind_of_asset(sources.asset_kinds, it->path);
			if (!kind.empty() && row_kind != kind) continue;
			if (static_cast<std::int64_t>(items.size()) >= page_size) {
				more = true;
				break;
			}
			// The listing may be old by now: a file swapped for a link since the scan is not followed.
			std::error_code status_ec;
			if (fs::symlink_status(it->physical, status_ec).type() != fs::file_type::regular || status_ec) continue;
			std::error_code ec;
			const std::uintmax_t size = fs::file_size(it->physical, ec);
			json::object item;
			item["tag"] = json::string("res://" + it->path);
			item["kind"] = json::string(row_kind);
			item["size"] = ec ? 0u : static_cast<std::uint64_t>(size);
			item["layer"] = static_cast<std::uint64_t>(it->layer);
			items.push_back(std::move(item));
			last = it->path;
		}

		json::object out;
		out["items"] = std::move(items);
		out["scan_truncated"] = state.truncated;
		out["skipped_names"] = static_cast<std::uint64_t>(state.bad_names);
		if (more) out["next_cursor"] = json::string(std::format("{:x}.{}", filter, hex_encode(last)));
		return cmd::result::success(std::move(out));
	}

	// ---- selection.get -----------------------------------------------------------------------------------------

	cmd::result selection_get(const edt::state_command_sources& sources, const cmd::call&)
	{
		const edt::selection& selection = sources.selection.current();
		const edt::document* document = selection.document != 0 ? sources.documents.find(selection.document) : nullptr;
		if (!document) document = &sources.documents.scene();

		json::array items;
		if (!selection.empty()) {
			json::object item;
			item["ref"] = ref_json(selection.world, selection.path);
			item["readonly"] = selection.readonly;
			if (selection.target.empty()) {
				located at;
				cmd::result ignored;
				json::value ref = ref_json(selection.world, selection.path);
				if (locate(sources.documents.scene(), ref, at, ignored) && at.target.empty()) {
					item["name"] = safe_string(at.owner->name);
				}
			}
			items.push_back(std::move(item));
		}
		json::object out;
		out["document"] = document->id();
		out["generation"] = document->generation();
		out["items"] = std::move(items);
		return cmd::result::success(std::move(out));
	}

}

std::vector<edt::registration> edt::register_state_commands(cmd::command_registry& commands, const state_command_sources& sources)
{
	std::vector<registration> out;
	const std::string page_size_schema = std::format(
		R"({{"type":"integer","minimum":1,"maximum":{},"description":"items per page, at most {}, default {}"}})",
		state_query_max_page_size, state_query_max_page_size, state_query_default_page_size);

	cmd::command_desc query;
	query.id = "scene.query";
	query.summary = "Page through the nodes of a world of the open scene: ref, name and component keys. Filter by name, component or subtree; follow next_cursor for more";
	query.category = cmd::agent_category::read;
	query.args_schema = parse_schema(with_ref(R"({
		"type": "object",
		"properties": {
			"document": { "type": "integer", "minimum": 1, "description": "scene document id; default the open scene" },
			"world": { "type": "string", "description": "world name; default the active world" },
			"filter": {
				"type": "object",
				"properties": {
					"name_glob": { "type": "string", "description": "* and ? wildcards, case-insensitive; without them a substring" },
					"has_component": { "type": "array", "items": { "type": "string" }, "description": "component keys or types that must all be present" },
					"under": @REF@
				},
				"additionalProperties": false
			},
			"page_size": )" + page_size_schema + R"(,
			"cursor": { "type": "string", "description": "next_cursor of the previous page; stale after an edit" }
		},
		"additionalProperties": false
	})"), "scene.query args");
	query.result_schema = parse_schema(with_ref(R"({
		"type": "object",
		"properties": {
			"document": { "type": "integer" },
			"world": { "type": "string" },
			"generation": { "type": "integer" },
			"items": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"ref": @REF@,
						"name": { "type": "string" },
						"components": { "type": "array", "items": { "type": "string" } },
						"prefab": { "type": "string" }
					},
					"required": ["ref", "name", "components"]
				}
			},
			"next_cursor": { "type": "string" }
		},
		"required": ["document", "world", "generation", "items"]
	})"), "scene.query result");
	query.invoke = [sources](const cmd::call& request) { return scene_query(sources, request); };
	keep(out, commands.add(std::move(query)));

	cmd::command_desc get;
	get.id = "node.get";
	get.summary = "One node by ref: name, parent, children, transform and its components as desc JSON, plus the override records it holds";
	get.category = cmd::agent_category::read;
	get.args_schema = parse_schema(with_ref(R"({
		"type": "object",
		"properties": {
			"ref": @REF@,
			"components": { "type": "array", "items": { "type": "string" }, "description": "only these component keys or types; default all" }
		},
		"required": ["ref"],
		"additionalProperties": false
	})"), "node.get args");
	get.result_schema = parse_schema(with_ref(R"({
		"type": "object",
		"properties": {
			"ref": @REF@,
			"document": { "type": "integer" },
			"generation": { "type": "integer" },
			"world": { "type": "string" },
			"name": { "type": "string" },
			"parent": { "type": ["object", "null"] },
			"in_instance": { "type": "boolean" },
			"prefab": { "type": "string" },
			"transform": { "type": "object" },
			"components": { "type": "object" },
			"children": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"ref": @REF@,
						"name": { "type": "string" },
						"components": { "type": "array", "items": { "type": "string" } },
						"has_children": { "type": "boolean" }
					},
					"required": ["ref", "name", "components", "has_children"]
				}
			},
			"child_count": { "type": "integer" },
			"children_truncated": { "type": "boolean" },
			"overrides": { "type": "array", "items": { "type": "object" } }
		},
		"required": ["ref", "name", "parent", "components", "children", "overrides"]
	})"), "node.get result");
	get.invoke = [sources](const cmd::call& request) { return node_get(sources, request); };
	keep(out, commands.add(std::move(get)));

	cmd::command_desc schema;
	schema.id = "component.schema";
	schema.summary = "JSON Schema of a component type (its __type), generated from the desc schema: field keys, types, ranges, defaults";
	schema.category = cmd::agent_category::read;
	schema.args_schema = parse_schema(R"({
		"type": "object",
		"properties": { "type": { "type": "string", "minLength": 1 } },
		"required": ["type"],
		"additionalProperties": false
	})", "component.schema args");
	schema.result_schema = parse_schema(R"({
		"type": "object",
		"properties": {
			"type": { "type": "string" },
			"version": { "type": "integer" },
			"kind": { "type": "string" },
			"is_component": { "type": "boolean" },
			"schema": { "type": "object" }
		},
		"required": ["type", "version", "kind", "is_component", "schema"]
	})", "component.schema result");
	schema.invoke = [sources](const cmd::call& request) { return component_schema(sources, request); };
	keep(out, commands.add(std::move(schema)));

	cmd::command_desc assets;
	assets.id = "asset.query";
	assets.summary = "Page through the assets under res:// (all resource layers merged, sorted by path): tag, kind, size. Filter by glob, kind or directory";
	assets.category = cmd::agent_category::read;
	assets.args_schema = parse_schema(R"({
		"type": "object",
		"properties": {
			"glob": { "type": "string", "description": "on the res-relative path; * and ? wildcards, case-insensitive; without them a substring" },
			"kind": { "type": "string", "description": "asset kind id, e.g. prefab" },
			"directory": { "type": "string", "description": "res-relative directory to list under; default the root" },
			"page_size": )" + page_size_schema + R"(,
			"cursor": { "type": "string", "description": "next_cursor of the previous page" }
		},
		"additionalProperties": false
	})", "asset.query args");
	assets.result_schema = parse_schema(R"({
		"type": "object",
		"properties": {
			"items": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"tag": { "type": "string" },
						"kind": { "type": "string" },
						"size": { "type": "integer" },
						"layer": { "type": "integer" }
					},
					"required": ["tag", "kind", "size", "layer"]
				}
			},
			"next_cursor": { "type": "string" },
			"scan_truncated": { "type": "boolean" },
			"skipped_names": { "type": "integer" }
		},
		"required": ["items", "scan_truncated"]
	})", "asset.query result");
	assets.invoke = [sources](const cmd::call& request) { return asset_query(sources, request); };
	keep(out, commands.add(std::move(assets)));

	cmd::command_desc selection;
	selection.id = "selection.get";
	selection.summary = "The current selection of the editor as refs (none when nothing is selected)";
	selection.category = cmd::agent_category::read;
	selection.result_schema = parse_schema(with_ref(R"({
		"type": "object",
		"properties": {
			"document": { "type": "integer" },
			"generation": { "type": "integer" },
			"items": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"ref": @REF@,
						"name": { "type": "string" },
						"readonly": { "type": "boolean" }
					},
					"required": ["ref", "readonly"]
				}
			}
		},
		"required": ["document", "generation", "items"]
	})"), "selection.get result");
	selection.invoke = [sources](const cmd::call& request) { return selection_get(sources, request); };
	keep(out, commands.add(std::move(selection)));
	return out;
}
