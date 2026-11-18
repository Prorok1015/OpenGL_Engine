#include "edt_mesh_shape_commands.h"
#include "edt_mesh_io.h"
#include "edt_mesh_shape_asset.h"
#include "edt_code_commands.h"
#include "edt_file_menu.h"
#include "edt_forge_types.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <format>
#include <glm/vec3.hpp>
#include <memory>

namespace edt::mesh
{
	namespace
	{
		constexpr std::string_view default_folder = "shapes";
		// A vertex of the poly file is its id and three coordinates, and its faces name it again: never less than this many characters.
		constexpr std::size_t k_least_poly_bytes_per_vertex = 40;

		json::value parse_schema(std::string_view text)
		{
			boost::system::error_code ec;
			json::value parsed = json::parse(text, ec);
			ASSERT_MSG(!ec, "a shape.* schema is not JSON: {}", ec.message());
			return parsed;
		}

		std::string string_of(const json::value& value)
		{
			const json::string& text = value.as_string();
			return std::string(text.data(), text.size());
		}

		std::vector<std::byte> bytes_of(const std::string& text)
		{
			const auto* first = reinterpret_cast<const std::byte*>(text.data());
			return std::vector<std::byte>(first, first + text.size());
		}

		std::string text_of(const std::vector<std::byte>& bytes)
		{
			return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}

		std::string who(const cmd::origin& from)
		{
			switch (from.kind) {
			case cmd::origin_kind::agent: return "agent " + from.session;
			case cmd::origin_kind::test: return "test";
			case cmd::origin_kind::game: return "game";
			case cmd::origin_kind::ui: return "ui";
			}
			return "ui";
		}

		cmd::result refuse(const char* code, std::string message)
		{
			return cmd::result::failure(code, std::move(message));
		}

		bool ends_with(std::string_view text, std::string_view suffix)
		{
			return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
		}

		struct shape_paths
		{
			std::string poly;
			std::string geometry;
			std::string binary;
		};

		// Project-relative paths of the two files of one shape, checked by the rules of the desc.* commands.
		std::optional<shape_paths> paths_of_poly(const std::string& poly, std::string& error)
		{
			const agent_level_path named = check_agent_level_path(poly);
			if (!named.ok()) {
				error = named.error;
				return std::nullopt;
			}
			if (!ends_with(named.path, poly_file_suffix)) {
				error = "a shape's source is a *.poly.desc file";
				return std::nullopt;
			}
			shape_paths out;
			out.poly = named.path;
			const std::string stem = named.path.substr(0, named.path.size() - poly_file_suffix.size());
			out.geometry = stem + std::string(geometry_file_suffix);
			out.binary = stem + ".geom.bin";
			return out;
		}

		struct prepared_shape
		{
			cmd::result failure = cmd::result::success();
			bool failed = false;
			shape_asset_files files;
			shape_params params = box_params{};
		};

		prepared_shape prepare_shape(const shape_params& params, const std::string& geometry_path)
		{
			prepared_shape out;
			const std::size_t estimated = estimate_shape_vertices(params);
			if (estimated > k_desc_file_limit / k_least_poly_bytes_per_vertex) {
				out.failed = true;
				out.failure = refuse(cmd::codes::invalid_args,
					std::format("the shape is too large: about {} vertices would not fit a file of {} bytes (desc.read and shape.rebuild move at most that); lower the segments or rings", estimated, k_desc_file_limit));
				return out;
			}
			shape_asset_result built = build_shape_asset(params, res::tag("res://" + geometry_path));
			if (!built.files) {
				out.failed = true;
				out.failure = refuse(cmd::codes::invalid_args, std::move(built.error));
				return out;
			}
			if (built.files->poly.size() > k_desc_file_limit) {
				out.failed = true;
				out.failure = refuse(cmd::codes::invalid_args,
					std::format("the shape is too large: its source file may not pass {} bytes (desc.read and shape.rebuild move at most that); lower the segments", k_desc_file_limit));
				return out;
			}
			out.files = std::move(*built.files);
			out.params = params;
			return out;
		}

		// kind + params of a call. Fields the call leaves out keep the stored values while the kind stays the same, else the kind's defaults.
		std::optional<shape_params> params_of_call(const json::object& args, const std::optional<shape_params>& stored, std::string& error)
		{
			const json::value* kind = args.if_contains("kind");
			const json::value* params = args.if_contains("params");
			if (!kind && !stored) {
				error = "give a kind";
				return std::nullopt;
			}
			const std::string kind_name = kind ? string_of(*kind) : std::string(shape_kind_name(*stored));
			json::object merged;
			if (stored && kind_name == shape_kind_name(*stored)) merged = shape_params_to_json(*stored);
			if (params) {
				for (const auto& [field, value] : params->as_object()) merged[field] = value;
			}
			return shape_params_from_json(kind_name, merged, error);
		}

		class shape_commands
		{
		public:
			explicit shape_commands(shape_command_deps deps) : m_deps(std::move(deps)) {}

			cmd::result create(const cmd::call& call) const
			{
				const json::object& args = call.args.as_object();
				const std::string name = string_of(args.at("name"));
				const agent_level_path named = check_agent_name(name);
				if (!named.ok()) return refuse(cmd::codes::invalid_args, named.error);
				std::string folder = std::string(default_folder);
				if (const json::value* given = args.if_contains("folder")) {
					const agent_level_path checked = check_agent_folder(string_of(*given));
					if (!checked.ok()) return refuse(cmd::codes::invalid_args, checked.error);
					folder = checked.path;
				}
				std::string error;
				const std::string poly_path = folder.empty() ? name + std::string(poly_file_suffix) : folder + "/" + name + std::string(poly_file_suffix);
				const std::optional<shape_paths> paths = paths_of_poly(poly_path, error);
				if (!paths) return refuse(cmd::codes::invalid_args, error);

				const std::optional<shape_params> params = params_of_call(args, std::nullopt, error);
				if (!params) return refuse(cmd::codes::invalid_args, error);
				if (std::optional<std::string> why = write_blocked()) return refuse(cmd::codes::busy, std::move(*why));
				const prepared_shape ready = prepare_shape(*params, paths->geometry);
				if (ready.failed) return ready.failure;

				for (const std::string& path : { paths->poly, paths->geometry }) {
					const desc_file_read there = look_at(path);
					if (there.in_open_level) return refuse(cmd::codes::busy, std::format("'res://{}' is a name of the open level", path));
					if (!there.missing && there.error.empty() && !there.top_layer) {
						return refuse(cmd::codes::needs_input,
							std::format("'res://{}' is a file of the engine or a package (a built-in shape such as shapes/box): choose another name", path));
					}
					if (!there.missing) {
						return refuse(there.error.empty() ? cmd::codes::needs_input : cmd::codes::failed,
							there.error.empty() ? std::format("'res://{}' exists: shape.create never replaces a file; use shape.rebuild (a person confirms that)", path) : there.error);
					}
				}
				const res::tag poly_tag = tag_of(paths->poly);
				const res::tag geometry_tag = tag_of(paths->geometry);
				if (const std::string why = rnd::write_geometry_files(m_deps.res, geometry_tag, ready.files.data); !why.empty()) {
					return refuse(cmd::codes::failed, std::format("'res://{}' was not created: {}", paths->geometry, why));
				}
				const res::file_operation poly_made = m_deps.res.create_sync(poly_tag, bytes_of(ready.files.poly));
				if (!poly_made.ok) {
					const res::file_operation removed_desc = m_deps.res.remove(geometry_tag);
					const res::file_operation removed_binary = m_deps.res.remove(tag_of(paths->binary));
					return refuse(cmd::codes::failed, std::format("'res://{}' was not created: {}{}", paths->poly, poly_made.error,
						removed_desc.ok && removed_binary.ok ? std::string{} : std::format(" ('res://{}' was written and could not be taken back)", paths->geometry)));
				}
				changed(call, "shape.create", paths->poly, ready.files.poly.size() + ready.files.geometry.size());
				return success_of(*paths, ready.files, std::nullopt);
			}

			cmd::result rebuild(const cmd::call& call) const
			{
				plan_result plan = plan_rebuild(call);
				if (!plan.failure.ok) return plan.failure;
				const res::tag poly_tag = tag_of(plan.paths.poly);
				const res::tag geometry_tag = tag_of(plan.paths.geometry);
				if (!m_deps.res.store_sync(poly_tag, bytes_of(plan.ready.files.poly))) {
					return refuse(cmd::codes::failed, std::format("'res://{}' could not be written", plan.paths.poly));
				}
				m_deps.res.signal_changed(poly_tag);
				const std::string geometry_problem = rnd::write_geometry_files(m_deps.res, geometry_tag, plan.ready.files.data);
				if (!geometry_problem.empty()) {
					const bool restored = m_deps.res.store_sync(poly_tag, plan.old_poly);
					m_deps.res.signal_changed(poly_tag);
					return refuse(cmd::codes::failed, std::format("'res://{}' could not be written: {}{}", plan.paths.geometry, geometry_problem,
						restored ? std::string{} : std::format(" and 'res://{}' could not be put back: it holds the new parameters", plan.paths.poly)));
				}
				changed(call, "shape.rebuild", plan.paths.poly, plan.ready.files.poly.size() + plan.ready.files.geometry.size());
				return success_of(plan.paths, plan.ready.files, update_nodes(call, plan.paths, plan.ready.files));
			}

			cmd::result add(const cmd::call& call) const
			{
				const json::object& args = call.args.as_object();
				std::string error;
				const std::optional<shape_paths> paths = paths_of_poly(string_of(args.at("poly")), error);
				if (!paths) return refuse(cmd::codes::invalid_args, error);
				std::optional<glm::vec3> scale;
				if (const json::value* given = args.if_contains("scale")) {
					const json::array& items = given->as_array();
					if (items.size() != 3) return refuse(cmd::codes::invalid_args, "scale is [x, y, z]");
					glm::vec3 value{};
					for (std::size_t axis = 0; axis < 3; ++axis) {
						if (!items[axis].is_number()) return refuse(cmd::codes::invalid_args, "scale is [x, y, z]");
						value[static_cast<int>(axis)] = items[axis].to_number<float>();
					}
					if (std::string problem = shape_scale_problem(value); !problem.empty()) return refuse(cmd::codes::invalid_args, std::move(problem));
					scale = value;
				}
				const desc_file_read poly = look_at(paths->poly);
				if (poly.missing) return refuse(cmd::codes::not_found, std::format("no file 'res://{}': shape.create makes the shape", paths->poly));
				if (!poly.error.empty()) return refuse(cmd::codes::failed, poly.error);
				const desc_file_read geometry = look_at(paths->geometry);
				if (geometry.missing) return refuse(cmd::codes::not_found, std::format("no file 'res://{}': the shape has no geometry yet", paths->geometry));
				if (!geometry.error.empty()) return refuse(cmd::codes::failed, geometry.error);
				const std::optional<material_range> range = poly_text_range(text_of(poly.bytes), error);
				if (!range) return refuse(cmd::codes::failed, std::format("'res://{}': {}", paths->poly, error));

				json::object fields{ { "geometry", "res://" + paths->geometry }, { "vx_begin", 0 }, { "vx_end", range->vx_end },
					{ "ind_begin", 0 }, { "ind_end", range->ind_end } };
				if (const json::value* material = args.if_contains("material")) fields["material"] = *material;
				const std::string stem = paths->poly.substr(paths->poly.rfind('/') == std::string::npos ? 0 : paths->poly.rfind('/') + 1);
				json::object made;
				made["parent"] = args.at("parent");
				made["name"] = args.if_contains("name") ? args.at("name") : json::value(stem.substr(0, stem.size() - poly_file_suffix.size()));
				if (const json::value* index = args.if_contains("index")) made["index"] = *index;
				if (scale) made["scale"] = json::array{ scale->x, scale->y, scale->z };
				made["components"] = json::object{ { "mesh", json::object{ { "type", "mesh_node_desc" }, { "fields", std::move(fields) } } } };
				return m_deps.commands.invoke("scene.node.create", std::move(made), call.from);
			}

			std::optional<std::string> describe_rebuild(const cmd::call& call) const
			{
				const auto refused = [](std::string_view why) {
					return std::optional<std::string>("This request will be refused: " + edt::forge::escaped_text(why, 1024));
				};
				const plan_result plan = plan_rebuild(call);
				if (!plan.failure.ok) return refused(plan.failure.error);
				return std::format("Rebuild the shape\n  res://{}\n  res://{}\nfrom {}, {} vertices, {} indices.\n"
					"The old content of both files is not kept. If the open level uses the geometry, it changes at once (hot reload) and cannot be undone with undo.",
					edt::forge::escaped_text(plan.paths.poly, 1024), edt::forge::escaped_text(plan.paths.geometry, 1024),
					shape_kind_name(plan.ready.params), plan.ready.files.vertices, plan.ready.files.indices);
			}

		private:
			struct plan_result
			{
				cmd::result failure = cmd::result::success();
				shape_paths paths;
				prepared_shape ready;
				std::vector<std::byte> old_poly;
			};

			plan_result plan_rebuild(const cmd::call& call) const
			{
				plan_result out;
				const json::object& args = call.args.as_object();
				std::string error;
				const std::optional<shape_paths> paths = paths_of_poly(string_of(args.at("path")), error);
				if (!paths) {
					out.failure = refuse(cmd::codes::invalid_args, error);
					return out;
				}
				out.paths = *paths;
				if (std::optional<std::string> why = write_blocked()) {
					out.failure = refuse(cmd::codes::busy, std::move(*why));
					return out;
				}
				const desc_file_read poly = look_at(paths->poly);
				if (poly.missing) {
					out.failure = refuse(cmd::codes::not_found, std::format("no file 'res://{}': shape.create makes a new one", paths->poly));
					return out;
				}
				if (!poly.error.empty()) {
					out.failure = refuse(cmd::codes::failed, poly.error);
					return out;
				}
				if (poly.in_open_level || !poly.top_layer) {
					out.failure = refuse(poly.in_open_level ? cmd::codes::busy : cmd::codes::invalid_args,
						std::format("'res://{}' belongs to {}", paths->poly, poly.in_open_level ? "the open level" : "a lower layer (the engine or a package), not to the project's own res/"));
					return out;
				}
				if (string_of(args.at("version")) != poly.version) {
					cmd::result stale = refuse(cmd::codes::stale, std::format("'res://{}' changed since that version was read (now {})", paths->poly, poly.version));
					out.failure = std::move(stale);
					return out;
				}
				out.old_poly = poly.bytes;
				const shape_source_check source = check_shape_source(text_of(poly.bytes));
				if (source.state != shape_source::generated) {
					out.failure = refuse(cmd::codes::failed, std::format("'res://{}' is not rebuilt: {}", paths->poly, source.error));
					return out;
				}
				const std::optional<shape_params> params = params_of_call(args, source.generator, error);
				if (!params) {
					out.failure = refuse(cmd::codes::invalid_args, error);
					return out;
				}
				out.ready = prepare_shape(*params, paths->geometry);
				if (out.ready.failed) {
					out.failure = out.ready.failure;
					return out;
				}
				const desc_file_read geometry = look_at(paths->geometry);
				if (geometry.in_open_level) {
					out.failure = refuse(cmd::codes::busy, std::format("'res://{}' is a name of the open level", paths->geometry));
				} else if (!geometry.missing && !geometry.error.empty()) {
					out.failure = refuse(cmd::codes::failed, geometry.error);
				} else if (!geometry.missing && !geometry.top_layer) {
					out.failure = refuse(cmd::codes::invalid_args, std::format("'res://{}' belongs to a lower layer, not to the project's own res/", paths->geometry));
				}
				return out;
			}

			desc_file_read look_at(const std::string& path) const
			{
				return read_desc_file(m_deps.res, m_deps.descs, m_deps.layout, tag_of(path));
			}

			static res::tag tag_of(const std::string& path)
			{
				return res::tag("res://" + path);
			}

			std::optional<std::string> write_blocked() const
			{
				return m_deps.layout.write_blocker ? m_deps.layout.write_blocker() : std::nullopt;
			}

			void changed(const cmd::call& call, std::string_view command, std::string target, std::size_t bytes) const
			{
				egLOG_INFO("agent", "{} {} ({} bytes) by {}", command, target, bytes, who(call.from));
				if (m_deps.events && call.from.kind == cmd::origin_kind::agent) {
					m_deps.events->agent_action.emit(edt::agent_action_info{ std::string(command), std::move(target), bytes, call.from.session });
				}
			}

			// The nodes of the open scene that draw this geometry keep a range of vertices and indices: after a rebuild it is the new one.
			// `scene_update` says why they were not changed (no scene commands, a playing scene, a gesture); nodes of files that are not open
			// are not seen and keep the old range: component.set changes them with the range this answer gives.
			struct node_update
			{
				std::size_t updated = 0;
				std::string refused;
			};

			node_update update_nodes(const cmd::call& call, const shape_paths& paths, const shape_asset_files& files) const
			{
				json::object range{ { "geometry", "res://" + paths.geometry }, { "vx_begin", 0 }, { "vx_end", files.vertices },
					{ "ind_begin", 0 }, { "ind_end", files.indices } };
				const cmd::result done = m_deps.commands.invoke("shape.update_nodes", std::move(range), call.from);
				node_update out;
				if (!done.ok) {
					out.refused = done.error;
					return out;
				}
				if (const json::value* updated = done.value.as_object().if_contains("updated")) out.updated = updated->to_number<std::size_t>();
				return out;
			}

			static cmd::result success_of(const shape_paths& paths, const shape_asset_files& files, const std::optional<node_update>& nodes)
			{
				const std::vector<std::byte> poly = bytes_of(files.poly);
				json::object out{ { "poly", "res://" + paths.poly }, { "geometry", "res://" + paths.geometry },
					{ "version", edt::code_version_of(poly) }, { "vertices", files.vertices }, { "indices", files.indices },
					{ "vx_begin", 0 }, { "vx_end", files.vertices }, { "ind_begin", 0 }, { "ind_end", files.indices } };
				if (nodes) {
					out["nodes_updated"] = nodes->updated;
					if (!nodes->refused.empty()) out["scene_update"] = nodes->refused;
				}
				return cmd::result::success(std::move(out));
			}

			shape_command_deps m_deps;
		};
	}

	std::vector<registration> register_shape_commands(cmd::command_registry& commands, shape_command_deps deps)
	{
		auto impl = std::make_shared<shape_commands>(std::move(deps));
		std::vector<registration> out;

		constexpr std::string_view kinds = R"("kind":{"type":"string","enum":["box","plane","sphere","cylinder","capsule"]},
			"params":{"type":"object","description":"the fields of the kind (box: size [x,y,z]; plane: size [x,z], segments_x, segments_z; sphere: radius, segments, rings; cylinder: radius, height, segments; capsule: radius, height (the straight part), segments, hemisphere_rings); a field left out takes the kind's default; segments are 3..256"})";
		constexpr std::string_view result = R"({"type":"object","properties":{"poly":{"type":"string"},"geometry":{"type":"string"},
			"version":{"type":"string","description":"of the poly file: what shape.rebuild takes back"},"vertices":{"type":"integer"},"indices":{"type":"integer"},
			"vx_begin":{"type":"integer"},"vx_end":{"type":"integer"},"ind_begin":{"type":"integer"},"ind_end":{"type":"integer"},
			"nodes_updated":{"type":"integer","description":"nodes of the open scene that drew the geometry and now hold the new range"},
			"scene_update":{"type":"string","description":"why those nodes were not changed"}},
			"required":["poly","geometry","version","vertices","indices","vx_begin","vx_end","ind_begin","ind_end"]})";

		cmd::command_desc create;
		create.id = "shape.create";
		create.summary = "Create a parametric shape in the project's res/: <folder>/<name>.poly.desc (the editable source, editor only) and "
			"<folder>/<name>.geom.desc (the geometry a mesh node refers to); never replaces a file";
		create.args_schema = parse_schema(std::format(R"({{"type":"object","properties":{{"name":{{"type":"string","minLength":1}},
			"folder":{{"type":"string","description":"a folder of res://, default shapes"}},{}}},"required":["name","kind"],"additionalProperties":false}})", kinds));
		create.result_schema = parse_schema(result);
		create.mutates = true;
		create.category = cmd::agent_category::scene_edit;
		create.invoke = [impl](const cmd::call& call) { return impl->create(call); };
		registration added = commands.add(std::move(create));
		if (added.active()) out.push_back(std::move(added));

		cmd::command_desc add;
		add.id = "shape.add";
		add.summary = "Create a node under `parent` (a ref, as scene.node.create) with a mesh that draws the whole geometry file of a shape made by "
			"shape.create (poly: its source file) and `scale` as its size; the same creation and undo as scene.node.create";
		add.args_schema = parse_schema(R"({"type":"object","properties":{"poly":{"type":"string","minLength":1,"description":"shapes/a.poly.desc, with or without the res protocol"},
			"parent":{"type":"object","description":"a node ref, as in scene.node.create"},"name":{"type":"string","minLength":1},
			"scale":{"type":"array","items":{"type":"number"}},"material":{"type":"string","description":"a material desc tag"},
			"index":{"type":"integer","minimum":0}},"required":["poly","parent"],"additionalProperties":false})");
		add.result_schema = parse_schema(R"({"type":"object"})");
		add.mutates = true;
		add.category = cmd::agent_category::scene_edit;
		add.invoke = [impl](const cmd::call& call) { return impl->add(call); };
		registration added_node = commands.add(std::move(add));
		if (added_node.active()) out.push_back(std::move(added_node));

		cmd::command_desc rebuild;
		rebuild.id = "shape.rebuild";
		rebuild.summary = "Rebuild both files of a parametric shape from new parameters (a person confirms each time). `version` is the one shape.create "
			"or shape.rebuild gave, else `stale`. Without kind and params the stored ones are used. A mesh edited by hand is refused";
		rebuild.args_schema = parse_schema(std::format(R"({{"type":"object","properties":{{"path":{{"type":"string","minLength":1,"description":"shapes/a.poly.desc with or without the res protocol"}},
			"version":{{"type":"string","minLength":1}},{}}},"required":["path","version"],"additionalProperties":false}})", kinds));
		rebuild.result_schema = parse_schema(result);
		rebuild.mutates = true;
		rebuild.destructive = true;
		rebuild.category = cmd::agent_category::scene_edit;
		rebuild.risk = cmd::danger::confirm;
		rebuild.invoke = [impl](const cmd::call& call) { return impl->rebuild(call); };
		rebuild.describe = [impl](const cmd::call& call) { return impl->describe_rebuild(call); };
		registration rebuilt = commands.add(std::move(rebuild));
		if (rebuilt.active()) out.push_back(std::move(rebuilt));
		return out;
	}
}
