#include <snk/project_migration/pmg_scan.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	using prj::migration::file_class;

	// Worse first: the class of a file is its worst object's.
	int severity(file_class kind)
	{
		switch (kind) {
		case file_class::invalid: return 5;
		case file_class::newer: return 4;
		case file_class::unknown_type: return 3;
		case file_class::outdated: return 2;
		case file_class::missing_resource: return 1;
		case file_class::current: return 0;
		}
		return 0;
	}

	std::string lower(std::string_view text)
	{
		std::string out(text);
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	std::string escape_pointer_token(std::string_view token)
	{
		std::string out;
		out.reserve(token.size());
		for (const char c : token) {
			if (c == '~') out += "~0";
			else if (c == '/') out += "~1";
			else out += c;
		}
		return out;
	}

	struct read_result
	{
		std::string text;
		std::string error;
	};

	// `path` below `root`. Through prj::read_project_file: no link is followed on the way and a FIFO or a device is never
	// opened (the plan is made for an agent's call too). A file over `max_bytes` is not read.
	read_result read_file(const fs::path& root, const fs::path& path, std::uintmax_t max_bytes)
	{
		std::error_code ec;
		if (fs::is_regular_file(fs::symlink_status(path, ec))) {
			const std::uintmax_t size = fs::file_size(path, ec);
			if (!ec && size > max_bytes) return { {}, std::format("too big ({} bytes, at most {})", size, max_bytes) };
		}
		prj::project_file_text read = prj::read_project_file(root, path.lexically_relative(root), max_bytes);
		if (!read.error.empty()) return { {}, std::move(read.error) };
		if (!read.exists) return { {}, "is gone" };
		return { std::move(read.text), {} };
	}

	// The depth the desc loader allows (desc_adapter_t), so a file the engine loads is not refused here.
	json::value parse_desc_json(std::string_view text, boost::system::error_code& error)
	{
		json::parse_options options;
		options.max_depth = 128;
		return json::parse(text, error, {}, options);
	}

	// The version of an object as desc_system::migrate_typed reads it; nullopt for one it refuses.
	std::optional<int> version_of(const json::object& obj)
	{
		const auto* version = obj.if_contains("__version");
		if (!version) return 1;
		std::int64_t parsed = 0;
		if (version->is_int64()) {
			parsed = version->as_int64();
		} else if (version->is_uint64()) {
			if (version->as_uint64() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return std::nullopt;
			parsed = static_cast<std::int64_t>(version->as_uint64());
		} else {
			return std::nullopt;
		}
		if (parsed < 1 || parsed > std::numeric_limits<int>::max()) return std::nullopt;
		return static_cast<int>(parsed);
	}

	bool is_known(const desc::desc_system& descs, std::string_view type)
	{
		return descs.is_registered(type) || descs.find_schema(type) != nullptr;
	}

	// What the scan asks of the types: the game layer itself, or a snapshot of it taken on the main thread (#761). Made
	// from either where a desc_system or a type_catalog is passed.
	class type_source
	{
	public:
		type_source(const desc::desc_system& descs) : m_descs(&descs) {}
		type_source(const prj::migration::type_catalog& catalog) : m_catalog(&catalog) {}

		bool known(std::string_view type) const { return m_descs ? is_known(*m_descs, type) : m_catalog->is_known(type); }
		int current_version(std::string_view type) const
		{
			return m_descs ? m_descs->current_version(type) : m_catalog->current_version(type);
		}

	private:
		const desc::desc_system* m_descs = nullptr;
		const prj::migration::type_catalog* m_catalog = nullptr;
	};

	// What an importer keeps in a sidecar: neither the scan nor a step of resource_meta looks into it.
	constexpr std::string_view k_sidecar_settings = "settings";

	struct object_walker
	{
		const type_source& descs;
		std::vector<prj::migration::typed_object>& objects;
		std::string bad_version;
		// The root of a .resource: its pseudo-type resource_meta is known there only (EPIC-58 3.4).
		bool sidecar = false;

		void value(const json::value& v, const std::string& pointer)
		{
			if (v.is_object()) {
				object(v.as_object(), pointer);
			} else if (v.is_array()) {
				const auto& array = v.as_array();
				// Numeric payloads (geometry) never hold typed objects, as in desc_system::migrate_value.
				if (!array.empty() && array.front().is_number()) return;
				for (std::size_t i = 0; i < array.size(); ++i) {
					value(array[i], std::format("{}/{}", pointer, i));
				}
			}
		}

		void object(const json::object& obj, const std::string& pointer)
		{
			if (const auto* type = obj.if_contains("__type"); type && type->is_string()) {
				prj::migration::typed_object found;
				found.pointer = pointer;
				found.type = std::string{ type->as_string() };
				found.known = descs.known(found.type) || (sidecar && pointer.empty() && found.type == res::asset_meta_type);
				if (const auto version = version_of(obj)) {
					found.version = *version;
				} else if (bad_version.empty()) {
					bad_version = std::format("'{}' at '{}' has an invalid __version", found.type, pointer);
				}
				found.current = found.known ? descs.current_version(found.type) : 0;
				objects.push_back(std::move(found));
			}
			for (const auto& member : obj) {
				if (sidecar && pointer.empty() && std::string_view(member.key()) == k_sidecar_settings) continue;
				value(member.value(), std::format("{}/{}", pointer, escape_pointer_token(member.key())));
			}
		}
	};

	void set_invalid(prj::migration::scanned_file& file, std::string reason)
	{
		file.kind = file_class::invalid;
		file.reason = std::move(reason);
		file.objects.clear();
		file.unknown_types.clear();
	}

	// Reads and parses a desc file; on a refusal `file` is invalid and the result is null.
	json::value read_desc(prj::migration::scanned_file& file, const fs::path& project, const fs::path& path, std::uintmax_t max_bytes)
	{
		read_result read = read_file(project, path, max_bytes);
		if (!read.error.empty()) {
			set_invalid(file, std::move(read.error));
			return {};
		}
		boost::system::error_code error;
		json::value root = parse_desc_json(read.text, error);
		if (error) {
			set_invalid(file, std::format("not JSON: {}", error.message()));
			return {};
		}
		if (!root.is_object()) {
			set_invalid(file, "the root is not a JSON object");
			return {};
		}
		return root;
	}

	// The class of a desc file from its typed objects.
	void classify(prj::migration::scanned_file& file, const json::object& root, const type_source& descs, bool sidecar = false)
	{
		file.kind = file_class::current;
		file.reason.clear();
		file.objects.clear();
		file.unknown_types.clear();
		file.root_guid = !sidecar && root.contains("__guid");
		object_walker walker{ descs, file.objects, {}, sidecar };
		walker.object(root, "");
		if (!walker.bad_version.empty()) {
			set_invalid(file, std::move(walker.bad_version));
			return;
		}

		file_class kind = file_class::current;
		const auto raise = [&kind](file_class to) {
			if (severity(to) > severity(kind)) kind = to;
		};
		for (const auto& object : file.objects) {
			if (!object.known) {
				if (std::ranges::find(file.unknown_types, object.type) == file.unknown_types.end()) {
					file.unknown_types.push_back(object.type);
				}
				raise(file_class::unknown_type);
			} else if (object.version > object.current) {
				if (file.reason.empty()) {
					file.reason = std::format("'{}' at '{}' is version {}, newer than {}", object.type, object.pointer, object.version, object.current);
				}
				raise(file_class::newer);
			} else if (object.version < object.current) {
				raise(file_class::outdated);
			}
		}
		file.kind = kind;
	}

	void scan_desc(prj::migration::scanned_file& file, const fs::path& project, const fs::path& path, const type_source& descs,
		std::uintmax_t max_bytes)
	{
		const json::value root = read_desc(file, project, path, max_bytes);
		if (root.is_object()) classify(file, root.as_object(), descs);
	}

	std::string_view without_bom(std::string_view text)
	{
		return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
	}

	// The sidecar's root object, or null with `file` invalid.
	json::value parse_sidecar(prj::migration::scanned_file& file, std::string_view text)
	{
		boost::system::error_code error;
		json::value root = parse_desc_json(without_bom(text), error);
		if (error || !root.is_object()) {
			set_invalid(file, error ? std::format("not a .resource: not JSON: {}", error.message()) : std::string{ "not a .resource: the root is not a JSON object" });
			return {};
		}
		const json::value* type = root.as_object().if_contains("__type");
		if (!type || !type->is_string() || type->as_string() != res::asset_meta_type) {
			set_invalid(file, std::format("not a .resource: '__type' is not \"{}\"", res::asset_meta_type));
			return {};
		}
		return root;
	}

	// At most res::asset_meta_max_bytes, whatever `max_bytes` allows: a bigger .resource is one the editor's asset index
	// does not read, so the scan must not call it current.
	json::value read_sidecar(prj::migration::scanned_file& file, const fs::path& project, const fs::path& path, std::uintmax_t max_bytes,
		std::string& text)
	{
		read_result read = read_file(project, path, std::min<std::uintmax_t>(max_bytes, res::asset_meta_max_bytes));
		if (!read.error.empty()) {
			set_invalid(file, std::move(read.error));
			return {};
		}
		text = std::move(read.text);
		return parse_sidecar(file, text);
	}

	// res::read_asset_meta of an engine that reads `readable`. The reader takes asset_meta_version only; another
	// `readable` is the test seam of scan_options::resource_meta_version: the sidecar at that version is read with
	// the reader's rules, as the engine raised with its step would.
	res::asset_meta_result read_meta_as(std::string_view text, std::int64_t readable)
	{
		if (readable == res::asset_meta_version) return res::read_asset_meta(text);
		boost::system::error_code error;
		json::value root = json::parse(without_bom(text), error);
		if (error || !root.is_object()) return res::read_asset_meta(text);
		json::object& object = root.as_object();
		const json::value* version = object.if_contains("__version");
		if (version && version->is_int64() && version->as_int64() == readable) object["__version"] = res::asset_meta_version;
		else if (version && version->is_int64() && version->as_int64() > readable) object["__version"] = res::asset_meta_version + 1;
		return res::read_asset_meta(json::serialize(root));
	}

	void classify_sidecar_root(prj::migration::scanned_file& file, const json::object& root, std::string_view text, const type_source& descs,
		std::int64_t readable)
	{
		classify(file, root, descs, true);
		if (file.kind == file_class::newer) {
			file.reason = std::format("written by a newer engine: {}", file.reason);
			return;
		}
		// Every sidecar the steps call current must be one the engine reads (a step that does not raise
		// res::asset_meta_version would leave every migrated sidecar newer_than_engine to the asset database).
		if (file.kind != file_class::current) return;
		const res::asset_meta_result meta = read_meta_as(text, readable);
		if (meta) return;
		const std::string where = meta.error.field_path.empty() ? std::string{} : std::format(" at '{}'", meta.error.field_path);
		if (meta.error.kind == res::asset_meta_error_kind::newer_than_engine) {
			file.kind = file_class::newer;
			file.reason = std::format("the engine does not read it{}: {}", where, meta.error.message);
		} else {
			set_invalid(file, std::format("not a .resource{}: {}", where, meta.error.message));
		}
	}

	void scan_sidecar(prj::migration::scanned_file& file, const fs::path& project, const fs::path& path, const type_source& descs,
		const prj::migration::scan_options& options)
	{
		std::string text;
		const json::value root = read_sidecar(file, project, path, options.max_file_bytes, text);
		if (root.is_object()) classify_sidecar_root(file, root.as_object(), text, descs, options.resource_meta_version);
	}

	// Under `value`, every key `from` becomes `to`.
	void rename_keys(json::value& value, std::string_view from, std::string_view to)
	{
		if (value.is_object()) {
			json::object& object = value.as_object();
			if (auto found = object.find(from); found != object.end()) {
				json::value moved = std::move(found->value());
				object.erase(found);
				object[to] = std::move(moved);
			}
			for (auto& member : object) rename_keys(member.value(), from, to);
		} else if (value.is_array()) {
			for (json::value& item : value.as_array()) rename_keys(item, from, to);
		}
	}

	struct root_walk
	{
		prj::migration::scan_result& result;
		const fs::path& project;
		const type_source& descs;
		const prj::migration::scan_options& options;

		void refuse(const fs::path& path, std::string reason)
		{
			prj::migration::scanned_file file;
			file.path = fs_paths::generic_utf8_of(path.lexically_relative(project));
			egLOG_WARN("project/migrate", "{}: {}", file.path, reason);
			set_invalid(file, std::move(reason));
			result.files.push_back(std::move(file));
		}

		// `owners`: by the res::asset_name_key of every file a .resource of this folder belongs to, that .resource's name
		// (EPIC-57 4.2); of two in other letter cases, the first by name.
		void entry(const fs::path& path, const fs::file_status& status, const std::map<std::string, std::string>& owners)
		{
			// A content root "." is the project itself: its code, build output and manifest are not content.
			const bool at_project_root = path.parent_path() == project;
			const std::string name = fs_paths::utf8_of(path.filename());
			if (res::is_skipped_asset_entry(name, fs::is_directory(status), at_project_root)) return;
			// Only a real folder is entered: a link or a junction to one is reported, never followed.
			if (fs::is_directory(status)) {
				folder(path);
				return;
			}
			if (!fs::is_regular_file(status)) {
				refuse(path, fs::is_symlink(status) ? "a link: it is not followed" : "not a regular file");
				return;
			}

			prj::migration::scanned_file file;
			file.path = fs_paths::generic_utf8_of(path.lexically_relative(project));
			const std::string extension = lower(fs_paths::utf8_of(path.extension()));
			if (extension == ".resource") {
				scan_sidecar(file, project, path, descs, options);
			} else {
				const auto owner = owners.find(res::asset_name_key(name, res::asset_names_case_insensitive));
				file.has_resource = owner != owners.end();
				if (file.has_resource) file.resource_path = fs_paths::generic_utf8_of((path.parent_path() / fs_paths::path_from_utf8(owner->second)).lexically_relative(project));
				if (extension == ".desc") {
					scan_desc(file, project, path, descs, options.max_file_bytes);
				} else {
					file.kind = file.has_resource ? file_class::current : file_class::missing_resource;
				}
			}
			result.files.push_back(std::move(file));
		}

		// One folder at a time: a folder that cannot be listed is reported and the walk goes on with the others.
		void folder(const fs::path& dir)
		{
			std::error_code ec;
			fs::directory_iterator it(dir, fs::directory_options::none, ec);
			if (ec) {
				refuse(dir, std::format("the folder cannot be listed: {}", ec.message()));
				return;
			}
			std::vector<std::pair<fs::path, fs::file_status>> entries;
			for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
				std::error_code status_error;
				const fs::file_status status = it->symlink_status(status_error);
				entries.emplace_back(it->path(), status_error ? fs::file_status{ fs::file_type::unknown } : status);
			}
			if (ec) refuse(dir, std::format("the folder could not be listed to the end: {}", ec.message()));
			std::map<std::string, std::string> owners;
			for (const auto& [path, status] : entries) {
				const std::string name = fs_paths::utf8_of(path.filename());
				if (fs::is_regular_file(status) && res::is_asset_meta_sidecar(name)) {
					const auto [held, added] = owners.try_emplace(res::asset_name_key(res::asset_meta_owner(name), res::asset_names_case_insensitive), name);
					if (!added && name < held->second) held->second = name;
				}
			}
			for (const auto& [path, status] : entries) entry(path, status, owners);
		}
	};

	void scan_root(prj::migration::scan_result& result, const fs::path& project, const fs::path& content_root, const type_source& descs,
		const prj::migration::scan_options& options)
	{
		root_walk walk{ result, project, descs, options };
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(content_root, ec);
		if (ec || status.type() == fs::file_type::not_found) return;
		if (!fs::is_directory(status)) {
			walk.refuse(content_root, fs::is_symlink(status) ? "the content root is a link: it is not followed" : "the content root is not a folder");
			return;
		}
		// A folder on the way to the root may be a link out of the project.
		std::error_code project_error;
		std::error_code root_error;
		const fs::path real_project = fs::weakly_canonical(project, project_error);
		const fs::path real_root = fs::weakly_canonical(content_root, root_error);
		if (project_error || root_error || !fs_paths::is_inside(real_project, real_root)) {
			walk.refuse(content_root, "the content root is outside the project (a link on its way): it is not scanned");
			return;
		}
		walk.folder(content_root);
	}

	prj::migration::manifest_state scan_manifest(const fs::path& root, std::optional<prj::manifest>& manifest)
	{
		prj::migration::manifest_state state;
		const fs::path path = root / fs_paths::path_from_utf8(prj::k_manifest_file_name);
		read_result read = read_file(root, path, std::uintmax_t{ 1 } << 20);
		if (!read.error.empty()) {
			state.kind = file_class::invalid;
			state.reason = std::format("project.snkproj {}", read.error);
			return state;
		}
		boost::system::error_code error;
		const json::value raw = json::parse(read.text, error);
		if (!error && raw.is_object()) {
			const auto* format = raw.as_object().if_contains("format_version");
			if (format && format->is_uint64()) {
				state.kind = file_class::invalid;
				state.reason = "format_version is out of range";
				return state;
			}
			if (format && format->is_int64()) {
				state.format_version = static_cast<int>(std::clamp<std::int64_t>(format->as_int64(), 0, std::numeric_limits<int>::max()));
				if (format->as_int64() > prj::k_manifest_format_version) {
					state.kind = file_class::newer;
					state.reason = std::format("format_version {} is newer than {}", format->as_int64(), prj::k_manifest_format_version);
					return state;
				}
			}
			// Hints only (EPIC-58 2): the scan of the files stays the truth, so a malformed one is ignored.
			if (const auto* with = raw.as_object().if_contains("migrated_with"); with && with->is_string()) {
				state.migrated_with = std::string(with->as_string());
			}
			if (const auto* steps = raw.as_object().if_contains("applied_steps"); steps && steps->is_array()) {
				for (const json::value& step : steps->as_array()) {
					if (step.is_string()) state.applied_steps.emplace_back(step.as_string());
				}
			}
		}
		prj::manifest_result parsed = prj::parse_manifest(read.text);
		if (!parsed.data) {
			state.kind = file_class::invalid;
			state.reason = parsed.error ? prj::to_string(*parsed.error) : std::string{ "the manifest cannot be read" };
			return state;
		}
		state.format_version = parsed.data->format_version;
		state.engine_version = prj::to_string(parsed.data->engine);
		state.engine = prj::compare_engine_versions(parsed.data->engine, prj::current_engine_version());
		if (*state.engine == prj::version_match::project_newer) {
			state.kind = file_class::newer;
			state.reason = std::format("engine_version {} is newer than the engine", state.engine_version);
		} else if (*state.engine == prj::version_match::project_older) {
			state.kind = file_class::outdated;
		}
		manifest = std::move(parsed.data);
		return state;
	}

	void flatten(const json::value& value, const std::string& pointer, std::map<std::string, const json::value*>& leaves)
	{
		if (value.is_object()) {
			for (const auto& member : value.as_object()) {
				flatten(member.value(), std::format("{}/{}", pointer, escape_pointer_token(member.key())), leaves);
			}
		} else if (value.is_array() && !(value.as_array().empty() || value.as_array().front().is_number())) {
			const auto& array = value.as_array();
			for (std::size_t i = 0; i < array.size(); ++i) {
				flatten(array[i], std::format("{}/{}", pointer, i), leaves);
			}
		} else {
			// A numeric array (a vector, geometry) is one leaf: a file of inline geometry is not a map of millions of numbers.
			leaves.emplace(pointer, &value);
		}
	}

	prj::migration::diff_stat diff_of(const json::value& before, const json::value& after)
	{
		std::map<std::string, const json::value*> old_leaves;
		std::map<std::string, const json::value*> new_leaves;
		flatten(before, "", old_leaves);
		flatten(after, "", new_leaves);
		prj::migration::diff_stat stat;
		for (const auto& [pointer, value] : new_leaves) {
			const auto found = old_leaves.find(pointer);
			if (found == old_leaves.end()) ++stat.added;
			else if (*found->second != *value) ++stat.changed;
		}
		for (const auto& [pointer, value] : old_leaves) {
			if (!new_leaves.contains(pointer)) ++stat.removed;
		}
		return stat;
	}

	std::vector<std::string> lines_of(const json::value& value)
	{
		const std::vector<std::byte> bytes = desc::pretty_json_bytes(value);
		const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		std::vector<std::string> lines;
		std::size_t start = 0;
		while (start < text.size()) {
			const std::size_t end = text.find('\n', start);
			const std::size_t stop = end == std::string::npos ? text.size() : end;
			std::string line = text.substr(start, stop - start);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(std::move(line));
			start = stop + 1;
		}
		return lines;
	}

	// ' ', '-' and '+' lines from a longest common subsequence; a pair of files too big for it is summarized.
	std::string line_diff(const json::value& before, const json::value& after)
	{
		const std::vector<std::string> a = lines_of(before);
		const std::vector<std::string> b = lines_of(after);
		constexpr std::size_t k_max_cells = std::size_t{ 4 } << 20;
		if (a.size() * b.size() > k_max_cells) {
			return std::format("(the diff is not computed: {} lines before, {} after)\n", a.size(), b.size());
		}
		std::vector<std::vector<std::uint32_t>> lcs(a.size() + 1, std::vector<std::uint32_t>(b.size() + 1, 0));
		for (std::size_t i = a.size(); i-- > 0;) {
			for (std::size_t j = b.size(); j-- > 0;) {
				lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
			}
		}
		std::string out;
		std::size_t i = 0;
		std::size_t j = 0;
		while (i < a.size() && j < b.size()) {
			if (a[i] == b[j]) {
				out += ' ' + a[i++] + '\n';
				++j;
			} else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
				out += '-' + a[i++] + '\n';
			} else {
				out += '+' + b[j++] + '\n';
			}
		}
		while (i < a.size()) out += '-' + a[i++] + '\n';
		while (j < b.size()) out += '+' + b[j++] + '\n';
		return out;
	}

	// "a/./b/" and "a/b" the same way, so paths below a content root "." compare with the project root.
	fs::path normalized(const fs::path& path)
	{
		fs::path out = path.lexically_normal();
		if (!out.has_filename() && out.has_parent_path()) out = out.parent_path();
		return out;
	}

	void add_step(std::vector<prj::migration::step_use>& steps, const std::string& type, int from, const std::string& summary)
	{
		const auto found = std::ranges::find_if(steps, [&](const auto& step) { return step.type == type && step.from_version == from; });
		if (found != steps.end()) {
			++found->objects;
			return;
		}
		steps.push_back(prj::migration::step_use{ type, from, summary, 1 });
	}
}

std::string_view prj::migration::to_string(file_class kind)
{
	switch (kind) {
	case file_class::current: return "current";
	case file_class::outdated: return "outdated";
	case file_class::newer: return "newer";
	case file_class::unknown_type: return "unknown_type";
	case file_class::invalid: return "invalid";
	case file_class::missing_resource: return "missing_resource";
	}
	return "invalid";
}

prj::migration::scanned_file prj::migration::classify_sidecar(std::string path, std::string_view text, const desc::desc_system& game_layer,
	std::int64_t readable_version)
{
	scanned_file file;
	file.path = std::move(path);
	const json::value root = parse_sidecar(file, text);
	if (root.is_object()) classify_sidecar_root(file, root.as_object(), text, game_layer, readable_version);
	return file;
}

void prj::migration::migrate_sidecar(const desc::desc_system& game_layer, json::object& root, std::string_view file,
	std::vector<desc::migration_warning>* warnings)
{
	constexpr std::string_view hidden = "\x1f__type";
	const auto settings = [&root]() { return root.if_contains(k_sidecar_settings); };
	if (json::value* own = settings()) rename_keys(*own, "__type", hidden);
	try {
		if (warnings) {
			game_layer.migrate_tree(root, file, *warnings);
		} else {
			game_layer.migrate_tree(root, file);
		}
	} catch (...) {
		if (json::value* own = settings()) rename_keys(*own, hidden, "__type");
		throw;
	}
	if (json::value* own = settings()) rename_keys(*own, hidden, "__type");
}

prj::migration::scanned_file prj::migration::classify_desc(std::string path, std::string_view text, const desc::desc_system& game_layer)
{
	scanned_file file;
	file.path = std::move(path);
	boost::system::error_code error;
	const json::value root = parse_desc_json(text, error);
	if (error) {
		set_invalid(file, std::format("not JSON: {}", error.message()));
	} else if (!root.is_object()) {
		set_invalid(file, "the root is not a JSON object");
	} else {
		classify(file, root.as_object(), game_layer);
	}
	return file;
}

std::size_t prj::migration::scan_result::count(file_class kind) const
{
	return static_cast<std::size_t>(std::ranges::count_if(files, [kind](const scanned_file& file) { return file.kind == kind; }));
}

const prj::migration::scanned_file* prj::migration::scan_result::find(std::string_view path) const
{
	const auto found = std::ranges::find_if(files, [path](const scanned_file& file) { return file.path == path; });
	return found == files.end() ? nullptr : &*found;
}

prj::migration::type_catalog prj::migration::type_catalog::from(const desc::desc_system& game_layer)
{
	type_catalog catalog;
	for (std::string& type : game_layer.type_names()) {
		// As is_known of the scan (a member of the same name hides it here).
		const entry value{ game_layer.is_registered(type) || game_layer.find_schema(type) != nullptr, game_layer.current_version(type) };
		catalog.add(std::move(type), value);
	}
	return catalog;
}

void prj::migration::type_catalog::add(std::string type, entry value)
{
	m_types.insert_or_assign(std::move(type), value);
}

bool prj::migration::type_catalog::is_known(std::string_view type) const
{
	const auto found = m_types.find(type);
	return found != m_types.end() && found->second.known;
}

int prj::migration::type_catalog::current_version(std::string_view type) const
{
	const auto found = m_types.find(type);
	return found == m_types.end() ? 1 : found->second.current_version;
}

prj::migration::scan_result prj::migration::scan_project(const fs::path& root, const desc::desc_system& game_layer, const scan_options& options)
{
	return scan_project(root, type_catalog::from(game_layer), options);
}

prj::migration::scan_result prj::migration::scan_project(const fs::path& root, const type_catalog& types, const scan_options& options)
{
	scan_result result;
	std::optional<prj::manifest> manifest;
	const fs::path project = normalized(root);
	result.manifest = scan_manifest(project, manifest);
	if (!manifest) return result;

	for (const auto& content_root : manifest->content_roots) {
		scan_root(result, project, normalized(project / content_root), types, options);
	}
	std::ranges::sort(result.files, {}, &scanned_file::path);

	// A .resource the scan calls invalid or newer is one the engine does not read: the file it pairs with has no ID there.
	std::map<std::string, file_class> sidecars;
	for (const scanned_file& file : result.files) {
		if (res::is_asset_meta_sidecar(file.path)) sidecars.emplace(file.path, file.kind);
	}
	for (scanned_file& file : result.files) {
		if (file.resource_path.empty()) continue;
		const auto sidecar = sidecars.find(file.resource_path);
		file.resource_unreadable = sidecar != sidecars.end() && (sidecar->second == file_class::invalid || sidecar->second == file_class::newer);
	}

	// What the editor lists has a .resource when the editor pairs one with it, in any content root (a file of a higher
	// root may take its .resource from a lower one); a file it does not list, one a higher root covers, by its own
	// folder. assets.ids decides by this alone, so its plan and what it does agree.
	project_assets assets(project, manifest->content_roots);
	assets.rebuild();
	for (scanned_file& file : result.files) {
		if (res::is_asset_meta_sidecar(file.path)) continue;
		const auto listed = assets.assets().find(file.path);
		if (listed == assets.assets().end()) continue;
		file.has_resource = listed->second.has_meta;
		if (file.resource_path.empty()) file.resource_unreadable = listed->second.has_meta && !listed->second.id;
		if (file.kind == file_class::current || file.kind == file_class::missing_resource) {
			if (lower(fs_paths::utf8_of(fs_paths::path_from_utf8(file.path).extension())) != ".desc") {
				file.kind = file.has_resource ? file_class::current : file_class::missing_resource;
			}
		}
	}
	return result;
}

prj::migration::migration_plan prj::migration::plan_migration(const fs::path& root, const scan_result& scan, const desc::desc_system& game_layer,
	const plan_options& options)
{
	migration_plan plan;
	plan.scan = scan;
	plan.resource_meta_version = options.scan.resource_meta_version;
	plan.force = options.force;
	if (const int steps_to = game_layer.current_version(res::asset_meta_type); steps_to != options.scan.resource_meta_version) {
		plan.refused = std::format("the steps of {} lead to version {}, but the engine reads version {}: a step must raise "
			"res::asset_meta_version with it", res::asset_meta_type, steps_to, options.scan.resource_meta_version);
	}
	for (scanned_file& file : plan.scan.files) {
		if (file.kind != file_class::outdated) continue;

		// The file as it is now, not as the scan saw it: its steps come from what will be migrated.
		const fs::path path = root / fs_paths::path_from_utf8(file.path);
		std::error_code status_error;
		if (!fs::is_regular_file(fs::symlink_status(path, status_error))) {
			set_invalid(file, "changed since the scan: not a regular file any more");
			continue;
		}
		json::value before;
		const bool sidecar = res::is_asset_meta_name(file.path);
		if (sidecar) {
			std::string text;
			before = read_sidecar(file, root, path, options.scan.max_file_bytes, text);
			if (!before.is_object()) continue;
			classify_sidecar_root(file, before.as_object(), text, game_layer, options.scan.resource_meta_version);
		} else {
			before = read_desc(file, root, path, options.scan.max_file_bytes);
			if (!before.is_object()) continue;
			classify(file, before.as_object(), game_layer);
		}
		if (file.kind != file_class::outdated) continue;

		file_plan planned;
		planned.path = file.path;
		json::value after = before;
		try {
			if (sidecar) {
				migrate_sidecar(game_layer, after.as_object(), file.path, &planned.warnings);
			} else {
				game_layer.migrate_tree(after.as_object(), file.path, planned.warnings);
			}
		} catch (const std::exception& e) {
			set_invalid(file, e.what());
			continue;
		}
		for (const typed_object& object : file.objects) {
			if (!object.known) continue;
			for (int from = object.version; from < object.current; ++from) {
				const std::string summary = game_layer.migration_summary(object.type, from);
				add_step(planned.steps, object.type, from, summary);
				add_step(plan.steps, object.type, from, summary);
			}
		}
		planned.diff = diff_of(before, after);
		if (options.full_diff) planned.full_diff = line_diff(before, after);
		plan.files.push_back(std::move(planned));
	}

	if (options.project_steps) {
		for (const project_phase phase : { project_phase::pre, project_phase::post }) {
			for (const project_step& step : options.project_steps->steps()) {
				if (step.phase != phase || !step.needed(plan.scan)) continue;
				plan.project_steps.push_back(planned_project_step{ step.id, std::string(to_string(phase)), step.summary });
			}
		}
	}
	return plan;
}
