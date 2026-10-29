#include <snk/project/prj_cmake_presets.h>
#include <snk/project/prj_json_text.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <utility>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/stream_parser.hpp>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::string_view k_user_prefix = "user-";

	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	json::string_view jsv(std::string_view text)
	{
		return json::string_view(text.data(), text.size());
	}

	std::string to_std(json::string_view text)
	{
		return { text.data(), text.size() };
	}

	std::optional<std::string> parse_json(std::string_view text, json::value& out)
	{
		json::stream_parser parser;
		json::error_code ec;
		std::size_t offset = parser.write(text.data(), text.size(), ec);
		if (!ec) {
			parser.finish(ec);
			offset = text.size();
		}
		if (ec) {
			std::size_t line = 1;
			std::size_t column = 1;
			for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
				if (text[i] == '\n') {
					++line;
					column = 1;
				} else {
					++column;
				}
			}
			return std::format("line {}, column {}: {}", line, column, ec.message());
		}
		out = parser.release();
		return std::nullopt;
	}

	bool is_blank(std::string_view text)
	{
		return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
	}

	struct file_text
	{
		bool exists = false;
		std::string text;
		std::string error;
	};

	file_text read_file(const fs::path& file)
	{
		file_text result;
		std::error_code ec;
		if (!fs::exists(file, ec)) {
			if (ec) {
				result.error = std::format("cannot read '{}': {}", utf8_of(file), ec.message());
			}
			return result;
		}
		result.exists = true;
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			result.error = std::format("cannot read '{}'", utf8_of(file));
			return result;
		}
		result.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		if (in.bad()) {
			result.error = std::format("cannot read '{}'", utf8_of(file));
		}
		return result;
	}

	std::optional<std::string> write_atomically(const fs::path& link_or_file, std::string_view text)
	{
		// A linked file is replaced at its target; renaming onto the link would turn it into a plain file.
		fs::path file = link_or_file;
		std::error_code link_ec;
		if (fs::is_symlink(link_or_file, link_ec)) {
			file = fs::canonical(link_or_file, link_ec);
			if (link_ec) {
				return std::format("'{}' links to a file that cannot be reached: {}", utf8_of(link_or_file), link_ec.message());
			}
		}
		std::random_device random;
		fs::path temp_name{ "." };
		temp_name += file.filename();
		temp_name += std::format(".{:08x}.tmp", random());
		const fs::path temp = file.parent_path() / temp_name;
		std::error_code ignored;
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.close();
			if (out.fail()) {
				fs::remove(temp, ignored);
				return std::format("cannot write '{}'", utf8_of(temp));
			}
		}
		std::error_code ec;
		fs::rename(temp, file, ec);
		if (ec) {
			fs::remove(temp, ignored);
			return std::format("cannot replace '{}': {}", utf8_of(file), ec.message());
		}
		return std::nullopt;
	}

	std::vector<std::string> inherits_of(const json::object& preset)
	{
		std::vector<std::string> parents;
		const json::value* inherits = preset.if_contains("inherits");
		if (!inherits) {
			return parents;
		}
		if (inherits->is_string()) {
			parents.push_back(to_std(inherits->get_string()));
		} else if (inherits->is_array()) {
			for (const auto& parent : inherits->get_array()) {
				if (parent.is_string()) {
					parents.push_back(to_std(parent.get_string()));
				}
			}
		}
		return parents;
	}

	// nullopt for null: CMake then unsets the inherited variable.
	std::optional<std::string> cache_value_text(const json::value& value)
	{
		const json::value* plain = &value;
		if (value.is_object()) {
			plain = value.get_object().if_contains("value");
			if (!plain) {
				return std::string{};
			}
		}
		if (plain->is_null()) {
			return std::nullopt;
		}
		if (plain->is_string()) {
			return to_std(plain->get_string());
		}
		if (plain->is_bool()) {
			return std::string{ plain->get_bool() ? "TRUE" : "FALSE" };
		}
		return json::serialize(*plain);
	}

	std::optional<int> int_of(const json::value& value)
	{
		if (value.is_int64()) {
			return static_cast<int>(std::clamp<std::int64_t>(value.get_int64(), INT32_MIN, INT32_MAX));
		}
		if (value.is_uint64()) {
			return static_cast<int>(std::min<std::uint64_t>(value.get_uint64(), INT32_MAX));
		}
		return std::nullopt;
	}

	std::string string_field(const json::object& object, std::string_view key)
	{
		const json::value* field = object.if_contains(jsv(key));
		return field && field->is_string() ? to_std(field->get_string()) : std::string{};
	}

	class configure_resolver
	{
	public:
		explicit configure_resolver(const json::array* presets)
		{
			if (!presets) {
				return;
			}
			for (const auto& item : *presets) {
				if (item.is_object()) {
					const std::string name = string_field(item.get_object(), "name");
					if (!name.empty() && !m_by_name.contains(name)) {
						m_by_name.emplace(name, &item.get_object());
					}
				}
			}
		}

		std::string effective_string(const std::string& name, std::string_view key) const
		{
			std::vector<std::string> visiting;
			const json::value* found = find_field(name, key, visiting);
			return found && found->is_string() ? to_std(found->get_string()) : std::string{};
		}

		std::map<std::string, std::string> effective_cache(const std::string& name) const
		{
			std::map<std::string, std::string> out;
			std::vector<std::string> visiting;
			merge_cache(name, out, visiting);
			return out;
		}

	private:
		const json::object* find(const std::string& name) const
		{
			const auto it = m_by_name.find(name);
			return it == m_by_name.end() ? nullptr : it->second;
		}

		static bool enter(std::vector<std::string>& visiting, const std::string& name)
		{
			if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
				return false;
			}
			visiting.push_back(name);
			return true;
		}

		const json::value* find_field(const std::string& name, std::string_view key, std::vector<std::string>& visiting) const
		{
			const json::object* preset = find(name);
			if (!preset || !enter(visiting, name)) {
				return nullptr;
			}
			const json::value* found = preset->if_contains(jsv(key));
			for (const auto& parent : inherits_of(*preset)) {
				if (found) {
					break;
				}
				found = find_field(parent, key, visiting);
			}
			visiting.pop_back();
			return found;
		}

		void merge_cache(const std::string& name, std::map<std::string, std::string>& out, std::vector<std::string>& visiting) const
		{
			const json::object* preset = find(name);
			if (!preset || !enter(visiting, name)) {
				return;
			}
			// The first parent listed wins over the later ones, the preset's own entries over all of them.
			const std::vector<std::string> parents = inherits_of(*preset);
			for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
				merge_cache(*it, out, visiting);
			}
			if (const json::value* cache = preset->if_contains("cacheVariables"); cache && cache->is_object()) {
				for (const auto& [key, value] : cache->get_object()) {
					if (auto text = cache_value_text(value)) {
						out[to_std(key)] = std::move(*text);
					} else {
						out.erase(to_std(key));
					}
				}
			}
			visiting.pop_back();
		}

		std::map<std::string, const json::object*> m_by_name;
	};

	const json::array* array_field(const json::object& object, std::string_view key)
	{
		const json::value* field = object.if_contains(jsv(key));
		return field && field->is_array() ? &field->get_array() : nullptr;
	}

	const json::object* find_named(const json::value& doc, std::string_view array_key, std::string_view name)
	{
		if (!doc.is_object()) {
			return nullptr;
		}
		const json::array* presets = array_field(doc.get_object(), array_key);
		if (!presets) {
			return nullptr;
		}
		for (const auto& item : *presets) {
			if (item.is_object() && string_field(item.get_object(), "name") == name) {
				return &item.get_object();
			}
		}
		return nullptr;
	}

	json::object* find_named(json::value& doc, std::string_view array_key, std::string_view name)
	{
		return const_cast<json::object*>(find_named(std::as_const(doc), array_key, name));
	}

	class user_doc_editor
	{
	public:
		user_doc_editor(json::value& doc, const prj::main_presets& main) : m_doc(doc), m_main(main) {}

		std::optional<std::string> ensure(const std::string& cfg)
		{
			if (!m_main.find_configure(cfg)) {
				return std::format("CMakePresets.json has no configure preset '{}'", cfg);
			}
			if (auto problem = prepare_document()) {
				return problem;
			}
			const std::string name = prj::user_preset_name(cfg);
			json::object& doc = m_doc.get_object();
			if (auto problem = check_array(doc, "configurePresets")) {
				return problem;
			}
			if (auto problem = check_array(doc, "buildPresets")) {
				return problem;
			}

			if (json::object* preset = find_named(m_doc, "configurePresets", name)) {
				if (!preset->contains("inherits")) {
					(*preset)["inherits"] = jsv(cfg);
				}
			} else {
				json::object created;
				created["name"] = jsv(name);
				created["inherits"] = jsv(cfg);
				created["binaryDir"] = prj::user_binary_dir(m_main, cfg);
				created["cacheVariables"] = json::object{};
				doc["configurePresets"].get_array().emplace_back(std::move(created));
			}

			// The team's build preset of that name carries its targets, configuration and cleanFirst over.
			const bool team_build = m_main.find_build(cfg) != nullptr;
			if (json::object* preset = find_named(m_doc, "buildPresets", name)) {
				if (team_build && !preset->contains("inherits")) {
					(*preset)["inherits"] = jsv(cfg);
				}
				if (!preset->contains("configurePreset")) {
					(*preset)["configurePreset"] = jsv(name);
				}
			} else {
				json::object created;
				created["name"] = jsv(name);
				if (team_build) {
					created["inherits"] = jsv(cfg);
				}
				created["configurePreset"] = jsv(name);
				doc["buildPresets"].get_array().emplace_back(std::move(created));
			}
			return std::nullopt;
		}

		int effective_version() const
		{
			int version = std::max(m_main.version, prj::k_min_presets_version);
			if (m_doc.is_object()) {
				if (const json::value* own = m_doc.get_object().if_contains("version")) {
					version = std::max(version, int_of(*own).value_or(0));
				}
			}
			return version;
		}

		std::optional<std::string> apply(const prj::ensure_user_preset& edit)
		{
			return ensure(edit.cfg);
		}

		std::optional<std::string> apply(const prj::set_define& edit)
		{
			if (auto problem = prj::check_user_define(m_main, edit.cfg, edit.name, edit.value, effective_version())) {
				return problem;
			}
			if (auto problem = ensure(edit.cfg)) {
				return problem;
			}
			json::object& preset = *find_named(m_doc, "configurePresets", prj::user_preset_name(edit.cfg));
			json::value& cache = preset["cacheVariables"];
			if (cache.is_null()) {
				cache = json::object{};
			}
			if (!cache.is_object()) {
				return std::format("cacheVariables of '{}' is not an object", prj::user_preset_name(edit.cfg));
			}
			json::value& entry = cache.get_object()[jsv(edit.name)];
			if (entry.is_object()) {
				entry.get_object()["value"] = jsv(edit.value);
			} else {
				entry = jsv(edit.value);
			}
			return std::nullopt;
		}

		std::optional<std::string> apply(const prj::remove_define& edit)
		{
			json::object* preset = find_named(m_doc, "configurePresets", prj::user_preset_name(edit.cfg));
			if (!preset) {
				return std::nullopt;
			}
			if (json::value* cache = preset->if_contains("cacheVariables"); cache && cache->is_object()) {
				cache->get_object().erase(jsv(edit.name));
			}
			return std::nullopt;
		}

		std::optional<std::string> apply(const prj::set_jobs& edit)
		{
			if (edit.jobs && *edit.jobs < 1) {
				return std::format("jobs must be at least 1, not {}", *edit.jobs);
			}
			if (auto problem = ensure(edit.cfg)) {
				return problem;
			}
			json::object& preset = *find_named(m_doc, "buildPresets", prj::user_preset_name(edit.cfg));
			if (edit.jobs) {
				preset["jobs"] = *edit.jobs;
			} else {
				preset.erase("jobs");
			}
			return std::nullopt;
		}

	private:
		static std::optional<std::string> check_array(json::object& doc, std::string_view key)
		{
			json::value& field = doc[jsv(key)];
			if (field.is_null()) {
				field = json::array{};
			}
			if (!field.is_array()) {
				return std::format("{} in CMakeUserPresets.json is not an array", key);
			}
			return std::nullopt;
		}

		std::optional<std::string> prepare_document()
		{
			const std::int64_t wanted = std::max(m_main.version, prj::k_min_presets_version);
			if (m_doc.is_null()) {
				json::object skeleton;
				skeleton["version"] = wanted;
				skeleton["configurePresets"] = json::array{};
				skeleton["buildPresets"] = json::array{};
				m_doc = std::move(skeleton);
				return std::nullopt;
			}
			if (!m_doc.is_object()) {
				return std::string{ "CMakeUserPresets.json is not a JSON object" };
			}
			json::object& doc = m_doc.get_object();
			if (json::value* version = doc.if_contains("version")) {
				const std::optional<int> current = int_of(*version);
				if (!current || *current < wanted) {
					*version = wanted;
				}
				return std::nullopt;
			}
			// A file without a version is broken for CMake; the version goes first, as CMake's own files have it.
			json::object reordered;
			reordered.reserve(doc.size() + 1);
			reordered["version"] = wanted;
			for (auto& member : doc) {
				reordered[member.key()] = std::move(member.value());
			}
			doc = std::move(reordered);
			return std::nullopt;
		}

		json::value& m_doc;
		const prj::main_presets& m_main;
	};

	std::string with_line_endings_of(std::string text, std::string_view original)
	{
		if (original.find("\r\n") == std::string_view::npos) {
			return text;
		}
		std::string out;
		out.reserve(text.size() + text.size() / 16);
		for (const char c : text) {
			if (c == '\n') {
				out += '\r';
			}
			out += c;
		}
		return out;
	}
}

const prj::configure_preset* prj::main_presets::find_configure(std::string_view name) const
{
	const auto it = std::find_if(configure.begin(), configure.end(), [&](const auto& preset) { return preset.name == name; });
	return it == configure.end() ? nullptr : &*it;
}

prj::main_presets_result prj::parse_cmake_presets(std::string_view text)
{
	main_presets_result result;
	json::value root;
	if (auto problem = parse_json(text, root)) {
		result.error = std::move(*problem);
		return result;
	}
	if (!root.is_object()) {
		result.error = "the presets file is not a JSON object";
		return result;
	}
	const json::object& doc = root.get_object();
	const json::value* version = doc.if_contains("version");
	const std::optional<int> version_number = version ? int_of(*version) : std::nullopt;
	if (!version_number) {
		result.error = "the presets file has no integer \"version\"";
		return result;
	}

	main_presets presets;
	presets.version = *version_number;
	const json::array* configure = array_field(doc, "configurePresets");
	const configure_resolver resolver(configure);
	if (configure) {
		for (const auto& item : *configure) {
			if (!item.is_object()) {
				continue;
			}
			const json::object& object = item.get_object();
			configure_preset preset;
			preset.name = string_field(object, "name");
			if (preset.name.empty()) {
				continue;
			}
			const json::value* hidden = object.if_contains("hidden");
			preset.hidden = hidden && hidden->is_bool() && hidden->get_bool();
			preset.display_name = string_field(object, "displayName");
			preset.generator = resolver.effective_string(preset.name, "generator");
			preset.binary_dir = resolver.effective_string(preset.name, "binaryDir");
			preset.inherits = inherits_of(object);
			preset.cache_variables = resolver.effective_cache(preset.name);
			presets.configure.push_back(std::move(preset));
		}
	}
	if (const json::array* build = array_field(doc, "buildPresets")) {
		for (const auto& item : *build) {
			if (!item.is_object()) {
				continue;
			}
			const json::object& object = item.get_object();
			build_preset preset;
			preset.name = string_field(object, "name");
			if (preset.name.empty()) {
				continue;
			}
			preset.configure_preset = string_field(object, "configurePreset");
			if (const json::value* jobs = object.if_contains("jobs")) {
				preset.jobs = int_of(*jobs);
			}
			presets.build.push_back(std::move(preset));
		}
	}
	result.presets = std::move(presets);
	return result;
}

prj::main_presets_result prj::load_cmake_presets(const fs::path& file)
{
	const file_text read = read_file(file);
	if (!read.error.empty() || !read.exists) {
		main_presets_result result;
		result.error = read.exists ? read.error : std::format("'{}' does not exist", utf8_of(file));
		return result;
	}
	main_presets_result result = parse_cmake_presets(read.text);
	if (!result.error.empty()) {
		result.error = std::format("'{}': {}", utf8_of(file), result.error);
	}
	return result;
}

const prj::build_preset* prj::main_presets::find_build(std::string_view name) const
{
	const auto it = std::find_if(build.begin(), build.end(), [&](const auto& preset) { return preset.name == name; });
	return it == build.end() ? nullptr : &*it;
}

std::optional<std::string> prj::invalid_preset_macro(std::string_view text, int presets_version)
{
	static constexpr std::string_view k_v3_macros[] = { "sourceDir", "sourceParentDir", "sourceDirName", "presetName",
		"generator", "hostSystemName", "dollar" };
	const auto known = [presets_version](std::string_view macro) {
		if (std::find(std::begin(k_v3_macros), std::end(k_v3_macros), macro) != std::end(k_v3_macros)) {
			return true;
		}
		return (macro == "fileDir" && presets_version >= 4) || (macro == "pathListSep" && presets_version >= 5);
	};
	constexpr std::string_view k_openers[] = { "{", "env{", "penv{", "vendor{" };
	for (std::size_t at = text.find('$'); at != std::string_view::npos; at = text.find('$', at + 1)) {
		const std::string_view rest = text.substr(at + 1);
		std::size_t open = std::string_view::npos;
		for (const std::string_view opener : k_openers) {
			if (rest.starts_with(opener)) {
				open = opener.size();
				break;
			}
		}
		const std::size_t close = open == std::string_view::npos ? std::string_view::npos : rest.find('}', open);
		const bool valid = close != std::string_view::npos && close > open && (open > 1 || known(rest.substr(open, close - open)));
		if (!valid) {
			const std::string_view shown = close == std::string_view::npos ? rest.substr(0, 16) : rest.substr(0, close + 1);
			return std::format("'${}' is not a CMake presets macro (presets version {}), and CMake would then refuse every "
				"preset; write ${{dollar}} for a literal '$', e.g. ${{dollar}}{{CMAKE_SOURCE_DIR}}", shown, presets_version);
		}
		at += close + 1;
	}
	return std::nullopt;
}

std::optional<std::string> prj::check_user_define(const main_presets& main, std::string_view cfg, std::string_view name,
	std::string_view value, int presets_version)
{
	if (name.empty()) {
		return std::string{ "a define needs a name" };
	}
	if (auto problem = invalid_preset_macro(name, presets_version)) {
		return std::format("define name: {}", *problem);
	}
	if (auto problem = invalid_preset_macro(value, presets_version)) {
		return std::format("define {}: {}", name, *problem);
	}
	if (const configure_preset* team = main.find_configure(cfg); team && team->cache_variables.contains(std::string{ name })) {
		return std::format("{} is set by the team preset '{}' in CMakePresets.json and is read-only here", name, cfg);
	}
	return std::nullopt;
}

std::string prj::user_preset_name(std::string_view cfg)
{
	std::string name{ k_user_prefix };
	name += cfg;
	return name;
}

std::string prj::user_binary_dir(const main_presets& main, std::string_view cfg)
{
	const configure_preset* preset = main.find_configure(cfg);
	if (!preset || preset->binary_dir.empty()) {
		return std::format("${{sourceDir}}/../build/{}", cfg);
	}
	constexpr std::string_view k_macro = "${presetName}";
	std::string dir = preset->binary_dir;
	for (std::size_t at = dir.find(k_macro); at != std::string::npos; at = dir.find(k_macro, at + cfg.size())) {
		dir.replace(at, k_macro.size(), cfg);
	}
	return dir;
}

std::optional<std::string> prj::apply_user_presets_edits(json::value& user_doc, const main_presets& main,
	const std::vector<user_presets_edit>& edits)
{
	user_doc_editor editor(user_doc, main);
	for (const auto& edit : edits) {
		if (auto problem = std::visit([&](const auto& op) { return editor.apply(op); }, edit)) {
			return problem;
		}
	}
	return std::nullopt;
}

bool prj::has_user_preset(const json::value& user_doc, std::string_view cfg)
{
	return find_named(user_doc, "configurePresets", user_preset_name(cfg)) != nullptr;
}

std::map<std::string, std::string> prj::user_defines(const json::value& user_doc, std::string_view cfg)
{
	std::map<std::string, std::string> defines;
	const json::object* preset = find_named(user_doc, "configurePresets", user_preset_name(cfg));
	if (!preset) {
		return defines;
	}
	if (const json::value* cache = preset->if_contains("cacheVariables"); cache && cache->is_object()) {
		for (const auto& [key, value] : cache->get_object()) {
			if (auto text = cache_value_text(value)) {
				defines.emplace(to_std(key), std::move(*text));
			}
		}
	}
	return defines;
}

std::optional<int> prj::user_jobs(const json::value& user_doc, std::string_view cfg)
{
	const json::object* preset = find_named(user_doc, "buildPresets", user_preset_name(cfg));
	if (!preset) {
		return std::nullopt;
	}
	const json::value* jobs = preset->if_contains("jobs");
	return jobs ? int_of(*jobs) : std::nullopt;
}

namespace
{
	prj::user_presets_read_result parse_user_presets(const fs::path& file, const file_text& read)
	{
		prj::user_presets_read_result result;
		if (!read.error.empty()) {
			result.error = read.error;
			return result;
		}
		if (!read.exists || is_blank(read.text)) {
			return result;
		}
		if (auto problem = parse_json(read.text, result.doc)) {
			result.doc = nullptr;
			result.error = std::format("'{}': {}", utf8_of(file), *problem);
			return result;
		}
		if (!result.doc.is_object()) {
			result.doc = nullptr;
			result.error = std::format("'{}' is not a JSON object", utf8_of(file));
		}
		return result;
	}
}

prj::user_presets_read_result prj::read_user_presets(const fs::path& code_dir)
{
	const fs::path file = code_dir / k_cmake_user_presets_file_name;
	return parse_user_presets(file, read_file(file));
}

prj::user_presets_save_result prj::save_user_presets(const fs::path& code_dir, const std::vector<user_presets_edit>& edits)
{
	user_presets_save_result result;
	const main_presets_result main = load_cmake_presets(code_dir / k_cmake_presets_file_name);
	if (!main.presets) {
		result.error = main.error;
		return result;
	}

	const fs::path file = code_dir / k_cmake_user_presets_file_name;
	const file_text original = read_file(file);
	user_presets_read_result read = parse_user_presets(file, original);
	if (!read.error.empty()) {
		result.error = std::move(read.error);
		return result;
	}

	for (const auto& edit : edits) {
		const std::string& cfg = std::visit([](const auto& op) -> const std::string& { return op.cfg; }, edit);
		const json::object* preset = find_named(std::as_const(read.doc), "configurePresets", user_preset_name(cfg));
		const json::value* dir = preset ? preset->if_contains("binaryDir") : nullptr;
		if (dir && dir->is_string()) {
			const std::string expected = user_binary_dir(*main.presets, cfg);
			if (to_std(dir->get_string()) != expected) {
				egLOG_WARN("project", "{}: binaryDir of '{}' is '{}', the tree of '{}' is '{}'; left as it is",
					utf8_of(file), user_preset_name(cfg), to_std(dir->get_string()), cfg, expected);
			}
		}
	}

	json::value doc = read.doc;
	if (auto problem = apply_user_presets_edits(doc, *main.presets, edits)) {
		result.error = std::move(*problem);
		return result;
	}
	// The same content keeps the file as the person formatted it.
	if (doc == read.doc) {
		return result;
	}
	const std::string text = with_line_endings_of(to_indented_json(doc), original.text);
	if (original.exists && text == original.text) {
		return result;
	}
	if (auto problem = write_atomically(file, text)) {
		result.error = std::move(*problem);
		return result;
	}
	result.written = true;
	return result;
}
