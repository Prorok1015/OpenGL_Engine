#include <snk/project_migration/pmg_scan.h>
#include <snk/common/fs_paths.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/project/prj_manifest.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <fstream>
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

	read_result read_file(const fs::path& path, std::uintmax_t max_bytes)
	{
		std::error_code ec;
		const std::uintmax_t size = fs::file_size(path, ec);
		if (ec) return { {}, std::format("cannot read its size: {}", ec.message()) };
		if (size > max_bytes) return { {}, std::format("too big ({} bytes, at most {})", size, max_bytes) };
		std::ifstream in(path, std::ios::binary);
		if (!in) return { {}, "cannot be opened" };
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (in.bad()) return { {}, "cannot be read" };
		return { std::move(text), {} };
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

	struct object_walker
	{
		const desc::desc_system& descs;
		std::vector<prj::migration::typed_object>& objects;
		std::string bad_version;

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
				found.known = is_known(descs, found.type);
				if (const auto version = version_of(obj)) {
					found.version = *version;
				} else if (bad_version.empty()) {
					bad_version = std::format("'{}' at '{}' has an invalid __version", found.type, pointer);
				}
				found.current = found.known ? descs.current_version(found.type) : 0;
				objects.push_back(std::move(found));
			}
			for (const auto& member : obj) {
				value(member.value(), std::format("{}/{}", pointer, escape_pointer_token(member.key())));
			}
		}
	};

	void scan_desc(prj::migration::scanned_file& file, const fs::path& path, const desc::desc_system& descs, std::uintmax_t max_bytes)
	{
		read_result read = read_file(path, max_bytes);
		if (!read.error.empty()) {
			file.kind = file_class::invalid;
			file.reason = std::move(read.error);
			return;
		}
		boost::system::error_code error;
		const json::value root = json::parse(read.text, error);
		if (error) {
			file.kind = file_class::invalid;
			file.reason = std::format("not JSON: {}", error.message());
			return;
		}
		if (!root.is_object()) {
			file.kind = file_class::invalid;
			file.reason = "the root is not a JSON object";
			return;
		}

		object_walker walker{ descs, file.objects, {} };
		walker.object(root.as_object(), "");
		if (!walker.bad_version.empty()) {
			file.kind = file_class::invalid;
			file.reason = std::move(walker.bad_version);
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

	bool is_hidden(const fs::path& name)
	{
		const std::string text = fs_paths::utf8_of(name);
		return !text.empty() && text.front() == '.';
	}

	// Until the .resource format reads its own files (#755, #768): a sidecar is <name>.resource next to the asset.
	bool has_sidecar(const fs::path& asset)
	{
		std::error_code ec;
		fs::path sidecar = asset;
		sidecar += ".resource";
		return fs::is_regular_file(fs::symlink_status(sidecar, ec));
	}

	void scan_root(prj::migration::scan_result& result, const fs::path& project, const fs::path& content_root, const desc::desc_system& descs,
		const prj::migration::scan_options& options)
	{
		std::error_code ec;
		const auto root_status = fs::symlink_status(content_root, ec);
		if (ec || !fs::is_directory(root_status)) return;

		fs::recursive_directory_iterator it(content_root, fs::directory_options::none, ec);
		const fs::recursive_directory_iterator end;
		for (; !ec && it != end; it.increment(ec)) {
			const fs::directory_entry& entry = *it;
			std::error_code status_error;
			const fs::file_status status = entry.symlink_status(status_error);
			const fs::path& path = entry.path();
			// A content root "." is the project itself: its code, build output and manifest are not content.
			const bool at_project_root = path.parent_path() == project;
			const std::string name = fs_paths::utf8_of(path.filename());
			if (is_hidden(path.filename()) || (at_project_root && (name == "code" || name == "build"))) {
				if (fs::is_directory(status)) it.disable_recursion_pending();
				continue;
			}
			if (fs::is_directory(status)) continue;
			if (at_project_root && name == "project.snkproj") continue;

			prj::migration::scanned_file file;
			file.path = fs_paths::generic_utf8_of(path.lexically_relative(project));
			if (status_error || !fs::is_regular_file(status)) {
				file.kind = file_class::invalid;
				file.reason = fs::is_symlink(status) ? "a link: it is not followed" : "not a regular file";
				result.files.push_back(std::move(file));
				continue;
			}
			const std::string extension = lower(fs_paths::utf8_of(path.extension()));
			if (extension == ".desc") {
				scan_desc(file, path, descs, options.max_file_bytes);
			} else if (extension == ".resource") {
				// Read and versioned once the format exists (#755, EPIC-58 S4).
				file.kind = file_class::current;
			} else {
				file.kind = has_sidecar(path) ? file_class::current : file_class::missing_resource;
			}
			result.files.push_back(std::move(file));
		}
	}

	prj::migration::manifest_state scan_manifest(const fs::path& root, std::optional<prj::manifest>& manifest)
	{
		prj::migration::manifest_state state;
		const fs::path path = root / "project.snkproj";
		read_result read = read_file(path, std::uintmax_t{ 1 } << 20);
		if (!read.error.empty()) {
			state.kind = file_class::invalid;
			state.reason = std::format("project.snkproj {}", read.error);
			return state;
		}
		boost::system::error_code error;
		const json::value raw = json::parse(read.text, error);
		if (!error && raw.is_object()) {
			if (const auto* format = raw.as_object().if_contains("format_version"); format && format->is_int64()) {
				state.format_version = static_cast<int>(std::clamp<std::int64_t>(format->as_int64(), 0, std::numeric_limits<int>::max()));
				if (format->as_int64() > prj::k_manifest_format_version) {
					state.kind = file_class::newer;
					state.reason = std::format("format_version {} is newer than {}", format->as_int64(), prj::k_manifest_format_version);
					return state;
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
		} else if (value.is_array()) {
			const auto& array = value.as_array();
			for (std::size_t i = 0; i < array.size(); ++i) {
				flatten(array[i], std::format("{}/{}", pointer, i), leaves);
			}
		} else {
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

std::size_t prj::migration::scan_result::count(file_class kind) const
{
	return static_cast<std::size_t>(std::ranges::count_if(files, [kind](const scanned_file& file) { return file.kind == kind; }));
}

const prj::migration::scanned_file* prj::migration::scan_result::find(std::string_view path) const
{
	const auto found = std::ranges::find_if(files, [path](const scanned_file& file) { return file.path == path; });
	return found == files.end() ? nullptr : &*found;
}

prj::migration::scan_result prj::migration::scan_project(const fs::path& root, const desc::desc_system& game_layer, const scan_options& options)
{
	scan_result result;
	std::optional<prj::manifest> manifest;
	const fs::path project = normalized(root);
	result.manifest = scan_manifest(project, manifest);
	if (!manifest) return result;

	for (const auto& content_root : manifest->content_roots) {
		scan_root(result, project, normalized(project / content_root), game_layer, options);
	}
	std::ranges::sort(result.files, {}, &scanned_file::path);
	return result;
}

prj::migration::migration_plan prj::migration::plan_migration(const fs::path& root, const scan_result& scan, const desc::desc_system& game_layer,
	const plan_options& options)
{
	migration_plan plan;
	plan.scan = scan;
	for (scanned_file& file : plan.scan.files) {
		if (file.kind != file_class::outdated) continue;

		read_result read = read_file(root / fs_paths::path_from_utf8(file.path), options.scan.max_file_bytes);
		boost::system::error_code error;
		json::value before;
		if (read.error.empty()) before = json::parse(read.text, error);
		if (!read.error.empty() || error || !before.is_object()) {
			file.kind = file_class::invalid;
			file.reason = read.error.empty() ? std::string{ "changed since the scan: not a JSON object any more" } : read.error;
			continue;
		}

		file_plan planned;
		planned.path = file.path;
		json::value after = before;
		try {
			game_layer.migrate_tree(after.as_object(), file.path, planned.warnings);
		} catch (const std::exception& e) {
			file.kind = file_class::invalid;
			file.reason = e.what();
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
	return plan;
}
