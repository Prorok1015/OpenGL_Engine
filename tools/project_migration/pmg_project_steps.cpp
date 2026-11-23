#include <snk/project_migration/pmg_project_steps.h>
#include <snk/common/fs_paths.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/render/rnd_material_migration.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_reference.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace prj::migration;

	bool manifest_format_needed(const scan_result& scan)
	{
		return scan.manifest.format_version < prj::k_manifest_format_version;
	}

	void manifest_format_apply(project_step_context& context)
	{
		const boost::json::value* before = context.manifest.if_contains("format_version");
		const std::string was = before ? boost::json::serialize(*before) : std::string{ "none" };
		context.manifest["format_version"] = prj::k_manifest_format_version;
		context.notes.push_back(std::format("project.snkproj: format_version {} -> {}", was, prj::k_manifest_format_version));
	}

	bool geometry_blobs_needed(const scan_result& scan)
	{
		return std::ranges::any_of(scan.files, &scanned_file::geometry_arrays);
	}

	// The pair engine.geometry_blobs writes for the desc `root` (the file at `path`), or why it leaves the desc as it is.
	struct prepared_geometry
	{
		rnd::geometry_desc geometry;
		rnd::geometry_files pair;
		std::string problem;
	};

	std::string_view text_of(const std::vector<std::byte>& bytes)
	{
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	bool same_layout(const rnd::driver::BufferLayout& a, const rnd::driver::BufferLayout& b)
	{
		return std::ranges::equal(a.get_elements(), b.get_elements(), [](const auto& x, const auto& y) {
			return x.Name == y.Name && x.Type == y.Type && x.Offset == y.Offset && x.Normalized == y.Normalized;
		});
	}

	// Why the pair would not give the data it was made of back, as the loader decodes it; empty when it does.
	std::string read_back_problem(const res::tag& tag, const rnd::geometry_files& pair, const rnd::geometry_desc& geometry)
	{
		boost::system::error_code error;
		const boost::json::value desc = boost::json::parse(text_of(pair.desc), error);
		if (error || !desc.is_object()) return "its new desc would not be a JSON object";
		rnd::geometry_desc read;
		if (std::string problem = rnd::decode_geometry_files(tag, desc.as_object(), pair.binary, read); !problem.empty()) {
			return std::format("its new pair would not read back: {}", problem);
		}
		if (read.vertices != geometry.vertices || read.indices != geometry.indices || !same_layout(read.layout, geometry.layout)) {
			return "its new pair would read back other data";
		}
		return {};
	}

	prepared_geometry prepare_geometry(std::string_view path, const boost::json::object& root)
	{
		prepared_geometry out;
		const res::tag tag{ path };
		if (std::string problem = rnd::decode_geometry_files(tag, root, {}, out.geometry); !problem.empty()) {
			out.problem = std::move(problem);
			return out;
		}
		// A desc with ranges stores no bounds, the loader builds them: stored ones that differ would be lost silently.
		if (!out.geometry.bounds.empty() && out.geometry.bounds != rnd::built_geometry_bounds(out.geometry)) {
			out.problem = "its stored bounds are not those the loader builds from its data, which a desc with a binary file keeps";
			return out;
		}
		rnd::geometry_file_options file_options;
		// The header (__guid, __parent, ...) as the file has it; the data stays out of the copy, the writer drops it anyway.
		for (const auto& member : root) {
			const std::string_view key = member.key();
			if (key == "vertices" || key == "indices" || key == "bounds" || key == "layout") continue;
			file_options.header[key] = member.value();
		}
		file_options.bounds = !out.geometry.bounds.empty();
		// The checks of the loader (rnd::geometry_write_problem) run in the encoder: refused data writes nothing.
		out.pair = rnd::encode_geometry_files(tag, out.geometry, file_options);
		if (!out.pair) {
			out.problem = out.pair.error;
		} else if (out.pair.binary.size() > k_geometry_binary_max_bytes) {
			out.problem = std::format("its binary file would be {} bytes, over the {} a migration writes", out.pair.binary.size(),
				k_geometry_binary_max_bytes);
		} else {
			out.problem = read_back_problem(tag, out.pair, out.geometry);
		}
		return out;
	}

	// Whether anything is at `path`; a name that cannot be checked (too large, unreadable) counts as taken.
	std::optional<std::string> taken(project_step_files& files, const std::string& path)
	{
		try {
			if (!files.exists(path)) return std::nullopt;
			return std::format("{} is already beside it", path);
		} catch (const std::exception& e) {
			return std::format("{} cannot be checked: {}", path, e.what());
		}
	}

	enum class geometry_outcome { converted, binary_kept };

	struct converted_geometry
	{
		geometry_outcome outcome = geometry_outcome::converted;
		// Why the desc stays as it is; empty when converted.
		std::string reason;
	};

	// Writes the pair of one desc. Nothing is written before every check passed.
	converted_geometry convert_geometry(project_step_files& files, const std::string& path)
	{
		const res::tag tag{ path };
		const std::optional<res::tag> binary = res::desc_binary::binary_tag(tag);
		const std::optional<res::tag> staged = res::desc_binary::staged_tag(tag);
		if (!binary || !staged) return { {}, "not named <name>.<ext>.desc: no binary file can stand beside it" };
		const std::string binary_path(binary->view());
		if (std::optional<std::string> reason = taken(files, std::string(staged->view()))) return { {}, *reason };

		const std::string read = files.read(path);
		std::string_view text = read;
		if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
		boost::system::error_code error;
		boost::json::parse_options options;
		options.max_depth = 128;
		const boost::json::value root = boost::json::parse(text, error, {}, options);
		if (error || !root.is_object()) return { {}, "no JSON object after the migration of its type" };
		const boost::json::object& desc = root.as_object();
		// The type migrations ran before: what the scan saw may be gone.
		if (!holds_convertible_geometry(path, desc)) return { {}, "no longer triangles in JSON arrays after the migration of its type" };
		const prepared_geometry prepared = prepare_geometry(path, desc);
		if (!prepared.problem.empty()) return { {}, prepared.problem };

		// The applier creates a file before its journal names it: a run stopped between the two leaves this very binary file
		// beside an unchanged desc, which the next run takes as it is.
		bool keep_binary = false;
		if (std::optional<std::string> reason = taken(files, binary_path)) {
			std::string beside;
			try {
				beside = files.read(binary_path);
			} catch (const std::exception&) {
				return { {}, *reason };
			}
			if (beside != text_of(prepared.pair.binary)) return { {}, *reason };
			keep_binary = true;
		}
		if (!keep_binary) files.create(binary_path, text_of(prepared.pair.binary));
		files.write(path, text_of(prepared.pair.desc));
		return { keep_binary ? geometry_outcome::binary_kept : geometry_outcome::converted, {} };
	}

	void geometry_blobs_apply(project_step_context& context)
	{
		std::size_t converted = 0;
		std::size_t left = 0;
		for (const scanned_file& file : context.scan.files) {
			if (!file.geometry_arrays) continue;
			const converted_geometry done = convert_geometry(context.files, file.path);
			if (!done.reason.empty()) {
				context.warn(std::format("{}: not migrated: {}", file.path, done.reason));
				++left;
				continue;
			}
			const std::string binary(res::desc_binary::binary_tag(res::tag{ file.path })->view());
			context.warn(done.outcome == geometry_outcome::binary_kept
				? std::format("{}: vertices and indices moved to {}, which an interrupted run had already written", file.path, binary)
				: std::format("{}: vertices and indices moved to {}", file.path, binary));
			++converted;
		}
		context.warn(std::format("{} geometry desc files with a binary file, {} not migrated", converted, left));
	}

	constexpr std::array<std::string_view, 3> k_stage_keys{ "shader_vertex", "shader_fragment", "shader_geometry" };
	constexpr std::array<std::string_view, 3> k_shader_keys{ "vertex", "fragment", "geometry" };
	constexpr std::string_view k_shader_suffix = ".shader.desc";
	constexpr int k_max_parents = 32;
	constexpr std::uint64_t k_max_desc_bytes = std::uint64_t{ 64 } << 20;

	using stage_triple = std::array<std::string, 3>;

	bool is_material_v1(const boost::json::object& object)
	{
		const boost::json::value* type = object.if_contains("__type");
		if (!type || !type->is_string() || type->as_string() != "material_desc") return false;
		const boost::json::value* version = object.if_contains("__version");
		if (!version) return true;
		if (version->is_int64()) return version->as_int64() == 1;
		return version->is_uint64() && version->as_uint64() == 1;
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

	using found_material = std::pair<std::string, const boost::json::object*>;

	void collect_materials(const boost::json::object& object, const std::string& pointer, std::vector<found_material>& out, int depth);

	void collect_materials(const boost::json::value& value, const std::string& pointer, std::vector<found_material>& out, int depth)
	{
		if (depth > 128) return;
		if (const boost::json::object* object = value.if_object()) {
			collect_materials(*object, pointer, out, depth);
		} else if (const boost::json::array* array = value.if_array()) {
			// Numeric payloads never hold typed objects, as the scan's walk.
			if (!array->empty() && array->front().is_number()) return;
			for (std::size_t i = 0; i < array->size(); ++i) collect_materials((*array)[i], std::format("{}/{}", pointer, i), out, depth + 1);
		}
	}

	void collect_materials(const boost::json::object& object, const std::string& pointer, std::vector<found_material>& out, int depth)
	{
		if (depth > 128) return;
		if (is_material_v1(object)) out.emplace_back(pointer, &object);
		for (const auto& member : object) collect_materials(member.value(), pointer + "/" + pointer_token(member.key()), out, depth + 1);
	}

	bool type_step_refuses(const boost::json::object& material, const desc::migration_id_lookup& id_of)
	{
		boost::json::object copy = material;
		std::vector<desc::migration_warning> warnings;
		desc::migration_context context("material_desc", 1, {}, &warnings, id_of);
		try {
			rnd::migrate_material_v1(copy, context);
		} catch (const std::runtime_error&) {
			return true;
		}
		return false;
	}

	// The engine's .shader.desc of a triple, by the type step itself: what it converts without a parent is an engine pair.
	std::optional<std::string> engine_shader_of(const stage_triple& stages)
	{
		boost::json::object probe;
		for (std::size_t i = 0; i < stages.size(); ++i) {
			if (!stages[i].empty()) probe[k_stage_keys[i]] = stages[i];
		}
		std::vector<desc::migration_warning> warnings;
		desc::migration_context context("material_desc", 1, {}, &warnings);
		try {
			rnd::migrate_material_v1(probe, context);
		} catch (const std::runtime_error&) {
			return std::nullopt;
		}
		const boost::json::value* shader = probe.if_contains("shader");
		if (!shader || !shader->is_string()) return std::nullopt;
		return std::string(shader->as_string());
	}

	boost::json::value parse_desc_text(std::string_view text, std::string_view what)
	{
		if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
		boost::system::error_code error;
		boost::json::parse_options options;
		options.max_depth = 128;
		boost::json::value root = boost::json::parse(text, error, {}, options);
		if (error || !root.is_object()) throw std::runtime_error(std::format("{} is no JSON object", what));
		return root;
	}

	// The descs a material's stages come from, as res:// stacks them: the project's files as this migration left them, then
	// the read-only layers (the engine's), highest first; engine:// names the lowest layer only.
	class desc_finder
	{
	public:
		explicit desc_finder(project_step_context& context)
			: m_context(context)
		{
			for (const auto& [path, asset] : context.assets.assets()) {
				m_project.emplace(res::asset_name_key(asset.res_path, res::asset_names_case_insensitive), path);
			}
		}

		// A {guid, path} names the file that holds its guid now, else its path.
		res::tag tag_of(const boost::json::value& value) const
		{
			if (value.is_string()) return res::tag{ std::string_view(value.as_string()) };
			const std::optional<res::reference> reference = res::reference_from_json(value);
			if (!reference) throw std::runtime_error(std::format("{} is no reference", boost::json::serialize(value)));
			if (!reference->guid.is_nil()) {
				if (std::optional<res::tag> held = m_context.assets.path_of(reference->guid)) return *held;
			}
			if (reference->path.view().empty()) {
				throw std::runtime_error(std::format("{} names an ID no file holds", boost::json::serialize(value)));
			}
			return reference->path;
		}

		// A stage as the material writes it: a text as it is, {guid, path} by its path (by its guid's file without one).
		std::string stage_text(const boost::json::value& value) const
		{
			if (value.is_null()) return {};
			if (value.is_string()) return std::string(value.as_string());
			if (const std::optional<res::reference> reference = res::reference_from_json(value); reference && !reference->path.view().empty()) {
				return reference->path.string();
			}
			return tag_of(value).string();
		}

		boost::json::object read(const res::tag& tag)
		{
			const std::string_view protocol = tag.protocol();
			const std::string relative(tag.relative());
			if (protocol == res::tag::default_protocol()) {
				const auto in_project = m_project.find(res::asset_name_key(relative, res::asset_names_case_insensitive));
				if (in_project != m_project.end()) {
					return parse_desc_text(m_context.files.read(in_project->second), in_project->second).as_object();
				}
			}
			if (protocol == res::tag::default_protocol() || protocol == res::tag::engine) {
				const std::vector<std::filesystem::path>& layers = m_context.assets.read_only_layers();
				const std::size_t count = protocol == res::tag::engine ? std::min<std::size_t>(layers.size(), 1) : layers.size();
				for (std::size_t i = count; i-- > 0;) {
					const std::filesystem::path& layer = protocol == res::tag::engine ? layers.front() : layers[i];
					const prj::project_file_text in_layer = prj::read_project_file(layer, fs_paths::path_from_utf8(relative), k_max_desc_bytes);
					if (!in_layer.error.empty()) throw std::runtime_error(std::format("{}: {}", tag.view(), in_layer.error));
					if (in_layer.exists) return parse_desc_text(in_layer.text, tag.view()).as_object();
				}
			}
			throw std::runtime_error(std::format("{} is neither in the project nor in the engine's files", tag.view()));
		}

		// Whether a read-only layer has a file at `relative` (below res://): a project file there would hide it.
		bool below(std::string_view relative) const
		{
			for (const std::filesystem::path& layer : m_context.assets.read_only_layers()) {
				const prj::project_file_text found = prj::read_project_file(layer, fs_paths::path_from_utf8(relative), k_max_desc_bytes);
				if (found.exists || !found.error.empty()) return true;
			}
			return false;
		}

	private:
		project_step_context& m_context;
		// res::asset_name_key of a res path -> the project file the walk lists for it.
		std::map<std::string, std::string> m_project;
	};

	// The stages a v1 material compiles with: its own shader_* keys (an empty one included), the others from its
	// __parent chain; a parent at v2 gives every stage still open from its "shader".
	stage_triple effective_stages(desc_finder& finder, const boost::json::object& material)
	{
		std::array<std::optional<std::string>, 3> stages;
		const auto take_own = [&](const boost::json::object& object) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				if (stages[i]) continue;
				if (const boost::json::value* value = object.if_contains(k_stage_keys[i])) stages[i] = finder.stage_text(*value);
			}
		};
		const auto complete = [&] { return std::ranges::all_of(stages, [](const auto& stage) { return stage.has_value(); }); };

		take_own(material);
		boost::json::object parent;
		const boost::json::object* current = &material;
		for (int depth = 0; !complete(); ++depth) {
			const boost::json::value* link = current->if_contains("__parent");
			if (!link) break;
			if (depth == k_max_parents) throw std::runtime_error(std::format("its parents go deeper than {}", k_max_parents));
			const res::tag tag = finder.tag_of(*link);
			boost::json::object next = finder.read(tag);
			parent = std::move(next);
			current = &parent;
			if (const boost::json::value* shader = parent.if_contains("shader"); shader && !shader->is_null()) {
				const boost::json::object program = finder.read(finder.tag_of(*shader));
				for (std::size_t i = 0; i < stages.size(); ++i) {
					if (stages[i]) continue;
					const boost::json::value* value = program.if_contains(k_shader_keys[i]);
					stages[i] = value ? finder.stage_text(*value) : std::string{};
				}
				break;
			}
			take_own(parent);
		}
		return { stages[0].value_or(""), stages[1].value_or(""), stages[2].value_or("") };
	}

	std::string where_of(const std::string& path, const std::string& pointer)
	{
		return pointer.empty() ? path : std::format("{} at {}", path, pointer);
	}

	// "a/rock.desc" -> "a/rock", "a/x.prefab.desc" -> "a/x.prefab".
	std::string stem_of(std::string_view path)
	{
		constexpr std::string_view suffix = ".desc";
		if (path.size() > suffix.size()) {
			std::string tail(path.substr(path.size() - suffix.size()));
			std::ranges::transform(tail, tail.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
			if (tail == suffix) return std::string(path.substr(0, path.size() - suffix.size()));
		}
		return std::string(path);
	}

	bool material_shaders_needed(const scan_result& scan)
	{
		return std::ranges::any_of(scan.files, &scanned_file::material_stages);
	}

	struct material_use
	{
		std::string file;
		std::string pointer;
		stage_triple stages;
	};

	struct shader_target
	{
		// Project-relative; empty for an engine file.
		std::string path;
		std::string tag;
		std::vector<std::string> users;
	};

	std::string shader_text(const stage_triple& stages)
	{
		boost::json::object shader;
		shader["__type"] = "shader_desc";
		shader["__version"] = 1;
		shader["vertex"] = stages[0];
		shader["fragment"] = stages[1];
		if (!stages[2].empty()) shader["geometry"] = stages[2];
		shader["defines"] = boost::json::array{};
		const std::vector<std::byte> bytes = desc::pretty_json_bytes(shader);
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	class shader_namer
	{
	public:
		shader_namer(project_step_context& context, desc_finder& finder)
			: m_context(context)
			, m_finder(finder)
		{
		}

		// The new .shader.desc beside `material` (project-relative): <stem>.shader.desc, else <stem>-2.shader.desc, ...
		shader_target name_beside(const std::string& material)
		{
			const auto listed = m_context.assets.assets().find(material);
			if (listed == m_context.assets.assets().end()) {
				throw std::runtime_error("the asset walk does not list it (a higher content root covers it): no res:// path to name a file beside it");
			}
			const std::string stem = stem_of(material);
			const std::string res_stem = stem_of(listed->second.res_path);
			for (int number = 1; number < 1000; ++number) {
				const std::string suffix = number == 1 ? std::string{} : std::format("-{}", number);
				shader_target target;
				target.path = stem + suffix + std::string(k_shader_suffix);
				const std::string res_path = res_stem + suffix + std::string(k_shader_suffix);
				target.tag = "res://" + res_path;
				if (taken(target.path, res_path)) continue;
				m_assigned.insert(key_of(target.path));
				return target;
			}
			throw std::runtime_error(std::format("no free name for {}{} up to -999", stem, k_shader_suffix));
		}

	private:
		static std::string key_of(std::string_view path) { return res::asset_name_key(path, res::asset_names_case_insensitive); }

		// A name that cannot be checked counts as taken: the step only creates, it never overwrites.
		bool taken(const std::string& path, const std::string& res_path)
		{
			if (m_assigned.contains(key_of(path))) return true;
			if (m_context.assets.assets().contains(path) || m_finder.below(res_path)) return true;
			try {
				return m_context.files.exists(path) || m_context.files.exists(path + ".resource");
			} catch (const std::exception&) {
				return true;
			}
		}

		project_step_context& m_context;
		desc_finder& m_finder;
		std::set<std::string> m_assigned;
	};

	void material_shaders_apply(project_step_context& context)
	{
		desc_finder finder(context);
		const desc::migration_id_lookup id_of = [&context](std::string_view path) { return context.assets.id_of(res::tag{ path }); };

		std::vector<std::string> paths;
		for (const scanned_file& file : context.scan.files) {
			if (file.material_stages) paths.push_back(file.path);
		}
		std::ranges::sort(paths);

		// Every stage is computed before anything is written: a parent this step rewrites is read as it was.
		std::map<std::string, boost::json::value> roots;
		std::vector<material_use> uses;
		std::size_t left = 0;
		for (const std::string& path : paths) {
			boost::json::value root;
			try {
				root = parse_desc_text(context.files.read(path), path);
			} catch (const std::exception& e) {
				context.warn(std::format("{}: not migrated: {}", path, e.what()));
				++left;
				continue;
			}
			for (const std::string& pointer : refused_material_stages(root.as_object(), id_of)) {
				try {
					stage_triple stages = effective_stages(finder, root.at_pointer(pointer).as_object());
					if (stages[0].empty() || stages[1].empty()) {
						throw std::runtime_error(std::format("its stages have {}: a .shader.desc needs both a vertex and a fragment stage",
							stages[0].empty() ? "no vertex" : "no fragment"));
					}
					uses.push_back(material_use{ path, pointer, std::move(stages) });
				} catch (const std::exception& e) {
					context.warn(std::format("{}: not migrated: {}", where_of(path, pointer), e.what()));
					++left;
				}
			}
			roots.emplace(path, std::move(root));
		}

		shader_namer namer(context, finder);
		std::map<stage_triple, std::size_t> by_stages;
		std::vector<shader_target> targets;
		std::vector<stage_triple> target_stages;
		std::set<std::string> changed;
		std::size_t migrated = 0;
		for (const material_use& use : uses) {
			auto found = by_stages.find(use.stages);
			if (found == by_stages.end()) {
				shader_target target;
				if (std::optional<std::string> engine = engine_shader_of(use.stages)) {
					target.tag = std::move(*engine);
				} else {
					try {
						target = namer.name_beside(use.file);
					} catch (const std::exception& e) {
						context.warn(std::format("{}: not migrated: {}", where_of(use.file, use.pointer), e.what()));
						++left;
						continue;
					}
				}
				found = by_stages.emplace(use.stages, targets.size()).first;
				targets.push_back(std::move(target));
				target_stages.push_back(use.stages);
			}
			shader_target& target = targets[found->second];
			target.users.push_back(where_of(use.file, use.pointer));
			boost::json::object& material = roots.at(use.file).at_pointer(use.pointer).as_object();
			for (const std::string_view key : k_stage_keys) material.erase(key);
			material["shader"] = target.tag;
			changed.insert(use.file);
			++migrated;
		}

		std::size_t created = 0;
		for (std::size_t i = 0; i < targets.size(); ++i) {
			shader_target& target = targets[i];
			std::ranges::sort(target.users);
			std::string users;
			for (const std::string& user : target.users) users += (users.empty() ? "" : ", ") + user;
			if (target.path.empty()) {
				context.warn(std::format("{}: the engine's shader, now referenced by {}", target.tag, users));
				continue;
			}
			context.files.create(target.path, shader_text(target_stages[i]));
			// A new ID now, so the material's reference gets one from assets.refs of this run.
			res::asset_meta meta;
			meta.guid = res::generate_asset_id();
			context.files.create(target.path + ".resource", res::write_asset_meta(meta));
			context.warn(std::format("{}: created, referenced by {}", target.path, users));
			++created;
		}
		for (const std::string& path : changed) {
			const std::vector<std::byte> bytes = desc::pretty_json_bytes(roots.at(path));
			context.files.write(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		}
		context.warn(std::format("{} materials, {} shader files, {} not migrated", migrated, created, left));
	}
}

bool prj::migration::holds_convertible_geometry(std::string_view path, const boost::json::object& root)
{
	const boost::json::value* type = root.if_contains("__type");
	if (!type || !type->is_string() || type->as_string() != "geometry_desc") return false;
	const boost::json::value* vertices = root.if_contains("vertices");
	const boost::json::value* indices = root.if_contains("indices");
	// No vertices: no mesh to write (the writer refuses an empty one), the arrays stay valid as they are.
	if (!vertices || !vertices->is_array() || vertices->as_array().empty() || !indices || !indices->is_array()) return false;
	if (rnd::topology_of(root) != rnd::geometry_topology::triangles) return false;
	return res::desc_binary::binary_tag(res::tag{ path }).has_value();
}

std::vector<std::string> prj::migration::refused_material_stages(const boost::json::object& root, const desc::migration_id_lookup& id_of)
{
	std::vector<found_material> materials;
	collect_materials(root, "", materials, 0);
	std::vector<std::string> pointers;
	for (const auto& [pointer, material] : materials) {
		if (type_step_refuses(*material, id_of)) pointers.push_back(pointer);
	}
	return pointers;
}

std::string prj::migration::geometry_conversion_problem(std::string_view path, const boost::json::object& root)
{
	return prepare_geometry(path, root).problem;
}

std::string_view prj::migration::to_string(project_phase phase)
{
	return phase == project_phase::pre ? "pre" : "post";
}

void prj::migration::project_step_registry::add(project_step step)
{
	if (step.id.empty() || !step.needed || !step.apply) {
		throw std::invalid_argument(std::format("project step '{}' needs an id, needed and apply", step.id));
	}
	if (find(step.id)) throw std::invalid_argument(std::format("project step '{}' is already registered", step.id));
	m_steps.push_back(std::move(step));
}

const prj::migration::project_step* prj::migration::project_step_registry::find(std::string_view id) const
{
	const auto found = std::ranges::find(m_steps, id, &project_step::id);
	return found == m_steps.end() ? nullptr : &*found;
}

void prj::migration::register_engine_project_steps(project_step_registry& registry)
{
	project_step format;
	format.id = "engine.manifest_format";
	format.phase = project_phase::pre;
	format.summary = std::format("project.snkproj to format_version {}", prj::k_manifest_format_version);
	format.needed = &manifest_format_needed;
	format.apply = &manifest_format_apply;
	registry.add(std::move(format));

	// Post: the type migrations come first (geometry_desc 1 -> 2), the step reads the file as they left it.
	project_step geometry;
	geometry.id = "engine.geometry_blobs";
	geometry.phase = project_phase::post;
	geometry.summary = "vertices and indices of geometry descs from JSON arrays to a binary file beside the desc";
	geometry.needed = &geometry_blobs_needed;
	geometry.apply = &geometry_blobs_apply;
	registry.add(std::move(geometry));

	// Pre: the type step refuses these materials, so the stages are rewritten before it runs; registered before the
	// asset steps, which then see the material's new reference.
	project_step shaders;
	shaders.id = "engine.material_shaders";
	shaders.phase = project_phase::pre;
	shaders.summary = "material shader stages the engine has no shader for to a .shader.desc beside the material";
	shaders.needed = &material_shaders_needed;
	shaders.apply = &material_shaders_apply;
	registry.add(std::move(shaders));
}
