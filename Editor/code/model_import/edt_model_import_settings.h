#pragma once
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The settings of a model import (#1195, docs/epics/editor/EPIC-62.md): what the `.resource` of the prefab an import
// writes records, what an agent's file.import_model may ask for and what the decoder process is told. No assimp here:
// the build without it reads and writes them too.
namespace edt {

	enum class model_normals : std::uint8_t { smooth, flat };

	// How the meshes are made, version 1. The defaults are the import as it was before #1195.
	struct model_import_settings
	{
		model_normals normals = model_normals::smooth;
		bool tangents = true;

		bool operator==(const model_import_settings&) const = default;
	};

	// The model a prefab was imported from: its file name and SHA-256, never its path (it lies outside the project).
	struct model_source_record
	{
		std::string name{};
		std::string sha256{};	// 64 lowercase hex digits

		bool operator==(const model_source_record&) const = default;
	};

	inline constexpr std::string_view k_model_importer = "model";
	inline constexpr std::int64_t k_model_settings_version = 1;
	inline constexpr std::size_t k_max_model_source_name_bytes = 255;

	std::string_view model_normals_name(model_normals normals);

	struct model_settings_parse
	{
		model_import_settings settings;
		std::optional<model_source_record> source;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	// What parse_model_settings does with a top-level key it does not know: an agent's arguments and the decoder's
	// request refuse it; a `.resource` read back ignores it (a newer editor of version 1 may have written it, and
	// rewrite_model_settings keeps it).
	enum class unknown_settings_keys : std::uint8_t { refuse, ignore };

	// An object; a wrong type or value is an error naming the key, a key of `source` it does not know too; a missing key
	// takes its default; `version`, when given, must be 1 (a higher one was written by a newer editor). A null value is
	// all defaults.
	model_settings_parse parse_model_settings(const boost::json::value& value,
		unknown_settings_keys unknown = unknown_settings_keys::refuse);
	// The record of a source file named `file_name`: the name as parse_model_settings takes it, every '/', '\\', ':',
	// control character and DEL made '_', cut to k_max_model_source_name_bytes on a UTF-8 boundary, "model" for an
	// empty, "." or ".." name. A model's file name may hold any of them ("scan:v2.glb" on Linux).
	model_source_record model_source_of(std::string_view file_name, std::string sha256);
	// What the `.resource` holds: {"version", "source"?, "normals", "tangents"}.
	boost::json::object model_settings_json(const model_import_settings& settings,
		const std::optional<model_source_record>& source = std::nullopt);
	// The settings of an existing `.resource` of importer "model" with `settings` and `source` put in: the keys this
	// editor does not know stay (a newer editor of version 1 may have written them). Empty when `out` holds them; else
	// why not (a version other than 1, or settings that are no object), and `out` is untouched.
	std::string rewrite_model_settings(const boost::json::value& existing, const model_import_settings& settings,
		const model_source_record& source, boost::json::object& out);
	// One line for a person: "normals smooth, tangents on".
	std::string model_settings_text(const model_import_settings& settings);

} // namespace edt
