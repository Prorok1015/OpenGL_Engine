#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>

// The game module's CMake presets (#444): <root>/code/CMakePresets.json is committed and never written by the editor;
// per-user settings go to CMakeUserPresets.json next to it, into presets named user-<cfg> that the editor owns.
namespace prj
{
	inline constexpr std::string_view k_cmake_presets_file_name = "CMakePresets.json";
	inline constexpr std::string_view k_cmake_user_presets_file_name = "CMakeUserPresets.json";
	inline constexpr int k_min_presets_version = 3;

	struct configure_preset
	{
		std::string name;
		bool hidden = false;
		std::string display_name;
		// generator, binary_dir and cache_variables are effective: resolved through `inherits`, the preset's own value
		// first, then its parents in the order listed. binary_dir is as written (macros such as ${presetName} kept).
		std::string generator;
		std::string binary_dir;
		std::vector<std::string> inherits;
		// A {type, value} entry is read as its value, a boolean as "TRUE"/"FALSE"; a null one unsets the inherited one.
		std::map<std::string, std::string> cache_variables;
	};

	struct build_preset
	{
		std::string name;
		std::string configure_preset;
		std::optional<int> jobs;
	};

	// Read-only view of CMakePresets.json.
	struct main_presets
	{
		int version = 0;
		std::vector<configure_preset> configure;
		std::vector<build_preset> build;

		SNK_API const configure_preset* find_configure(std::string_view name) const;
		SNK_API const build_preset* find_build(std::string_view name) const;
	};

	struct main_presets_result
	{
		std::optional<main_presets> presets;
		// Set exactly when `presets` is empty; a parse error names its line and column.
		std::string error;
	};

	SNK_API main_presets_result parse_cmake_presets(std::string_view json);
	SNK_API main_presets_result load_cmake_presets(const std::filesystem::path& file);

	// Edits of CMakeUserPresets.json, kept as a list so they can be applied again to the file as it is on disk at
	// save time. `cfg` names a configure preset of CMakePresets.json; the edits go to user-<cfg>.
	// user-<cfg> inherits <cfg>: the configure preset always, the build preset when CMakePresets.json has a build preset
	// named <cfg>.
	struct ensure_user_preset
	{
		std::string cfg;
	};
	// Refused (see check_user_define) for a name the team preset <cfg> already sets: team defines are read-only.
	struct set_define
	{
		std::string cfg;
		std::string name;
		std::string value;
	};
	// Does not unset the variable in an existing CMakeCache.txt, and user-<cfg> shares its build tree with <cfg>: the next
	// build must configure with --fresh (CMake 3.24+).
	struct remove_define
	{
		std::string cfg;
		std::string name;
	};
	// nullopt removes `jobs` (CMake's default).
	struct set_jobs
	{
		std::string cfg;
		std::optional<int> jobs;
	};
	using user_presets_edit = std::variant<ensure_user_preset, set_define, remove_define, set_jobs>;

	SNK_API std::string user_preset_name(std::string_view cfg);

	// The error for the first `$` in `text` that does not start a preset macro CMake knows at `presets_version`
	// (${sourceDir} ${sourceParentDir} ${sourceDirName} ${presetName} ${generator} ${hostSystemName} ${dollar}
	// $env{X} $penv{X} $vendor{X}; ${fileDir} from 4, ${pathListSep} from 5). CMake reads both presets files together
	// and one bad macro makes it refuse all of them.
	SNK_API std::optional<std::string> invalid_preset_macro(std::string_view text, int presets_version);
	// Why set_define{cfg, name, value} is refused: an empty name, an unknown macro in the name or value, or a name the
	// team preset <cfg> sets (effective cacheVariables of CMakePresets.json; read-only for the editor).
	SNK_API std::optional<std::string> check_user_define(const main_presets& main, std::string_view cfg,
		std::string_view name, std::string_view value, int presets_version);

	// The binaryDir of user-<cfg>: the effective binaryDir of `cfg` with ${presetName} replaced by `cfg`, so both build
	// in one tree; ${sourceDir}/../build/<cfg> when `cfg` has none.
	SNK_API std::string user_binary_dir(const main_presets& main, std::string_view cfg);

	// Changes `user_doc` in place: a null document becomes {version, configurePresets, buildPresets}; set_define and
	// set_jobs create user-<cfg> first. Only user-<cfg> presets and only the keys the editor writes are touched; the
	// rest (other presets, unknown fields, "$comment", order) stays. version is raised to the main file's (at least
	// k_min_presets_version), never lowered. An error leaves the edits before it applied.
	SNK_API std::optional<std::string> apply_user_presets_edits(boost::json::value& user_doc, const main_presets& main,
		const std::vector<user_presets_edit>& edits);

	SNK_API bool has_user_preset(const boost::json::value& user_doc, std::string_view cfg);
	// The cacheVariables of user-<cfg> itself (not inherited ones), read like configure_preset::cache_variables.
	SNK_API std::map<std::string, std::string> user_defines(const boost::json::value& user_doc, std::string_view cfg);
	SNK_API std::optional<int> user_jobs(const boost::json::value& user_doc, std::string_view cfg);

	struct user_presets_read_result
	{
		// Null when the file does not exist (or holds only whitespace).
		boost::json::value doc;
		std::string error;
	};

	// <code_dir>/CMakeUserPresets.json; a file that is not a JSON object is an error.
	SNK_API user_presets_read_result read_user_presets(const std::filesystem::path& code_dir);

	struct user_presets_save_result
	{
		bool written = false;
		std::string error;
	};

	// Reads CMakePresets.json and CMakeUserPresets.json from `code_dir` again, applies `edits` and writes the user file
	// (atomically, through a temporary file) only when its content changes. A missing or broken main file, or a broken
	// user file, is an error and nothing is written.
	SNK_API user_presets_save_result save_user_presets(const std::filesystem::path& code_dir,
		const std::vector<user_presets_edit>& edits);
}
