#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_tag.h>
#include <snk/resource/res_diagnostics.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace desc
{
	class desc_system;
}

namespace res
{
	class resource_system;
}

namespace edt
{
	class editor_events;

	// desc.read / desc.create / desc.write (#550): an agent reads and writes the desc files (*.desc: levels, materials,
	// prefabs...) of the project's res/ folder. Every path passes check_agent_level_path (inside the project, no link,
	// no drive or protocol, *.desc only) before the disk is touched; the content is a JSON object that must fit the
	// schemas of its `__type` objects before a byte is written. desc.write replaces an existing file and asks a person
	// each time; desc.create never replaces. Main thread only.
	struct desc_file_deps
	{
		desc_file_deps(res::resource_system& resources, const desc::desc_system& descriptions) : res(resources), descs(descriptions) {}

		res::resource_system& res;
		const desc::desc_system& descs;
		// The folder res:// writes into (the top layer of the project that is open now).
		std::function<std::filesystem::path()> top_layer;
		// Every folder res:// is read from, the top one last. Null: only top_layer.
		std::function<std::vector<std::filesystem::path>()> layers;
		// Project-relative paths of the files the open level lives in (levels/a.desc and its editor copy): they belong
		// to the document in memory, which would not follow a write. Null: none.
		std::function<std::vector<std::string>()> open_level_files;
		// agent_action after each change an agent made; null: none.
		editor_events* events = nullptr;
		// Why nothing may be written into the project now (a migration runs, or one stopped half way and is not rolled back,
		// #761): desc.create and desc.write answer busy with it. Null or nullopt: they write.
		std::function<std::optional<std::string>()> write_blocker;
		// Why the asset index cannot judge a guid now (not attached, files waiting for a hash or an ID): content with a
		// guid answers busy with it, since a verdict from it could be wrong either way (#968). Null or nullopt: judged now.
		std::function<std::optional<std::string>()> asset_index_wait;
		// The file a new document of `type` named `name` gets (document_creator::file_name_of: its asset kind's suffix). desc.create
		// refuses a material whose path is not that name, naming it (#1198). Null: any *.desc name is taken.
		std::function<std::string(std::string_view type, std::string_view name)> file_name_of;
	};

	[[nodiscard]] std::vector<registration> register_desc_file_commands(cmd::command_registry& commands, desc_file_deps deps);

	// Where the desc files of the project are: the same three functions as in desc_file_deps, for what edits a file without
	// holding the services (a data document).
	struct desc_file_layout
	{
		std::function<std::filesystem::path()> top_layer;
		std::function<std::vector<std::filesystem::path>()> layers;
		std::function<std::vector<std::string>()> open_level_files;
		// Why nothing may be written into the project now (a migration that runs, or one that stopped half way and is not
		// rolled back yet, #761): a data document's save refuses with it. Null or nullopt: it writes.
		std::function<std::optional<std::string>()> write_blocker = {};
		// As desc_file_deps::asset_index_wait; only the desc.* commands read it (a person's save needs no guid judged).
		std::function<std::optional<std::string>()> asset_index_wait = {};
	};

	// A desc file read the way desc.read does, with what desc.write asks before it replaces one: `tag` must be a res://
	// file named as check_agent_level_path wants (inside the project, *.desc, no link on the way), at most
	// k_desc_file_limit bytes (checked before the read), read fresh through the resolver. `error` is empty when the bytes
	// are there. `top_layer`: the file is in the project's own res/ (a lower layer, the engine or a package, is not
	// the project's to write); `in_open_level`: it is the open level's file or one of its worlds (the level document owns it).
	struct desc_file_read
	{
		std::vector<std::byte> bytes;
		std::string error;
		bool top_layer = false;
		bool in_open_level = false;
		// The path passed every check that needs no file (a res:// desc inside the project, no link on the way), whether
		// the file is there or not: `top_layer` and `in_open_level` are then known (a file that is nowhere is a new one in
		// the top layer). `missing`: there is no such file in any layer.
		bool located = false;
		bool missing = false;
		// For a file nobody has: why the open level makes its name not the project's to take (the name is the level's, or an entry of its
		// `worlds` cannot be read, so it cannot be told); `level_unreadable` marks the second.
		std::string level_refusal;
		bool level_unreadable = false;
		// code_version_of(bytes) when the bytes were read, whether or not they parse.
		std::string version;
	};
	desc_file_read read_desc_file(res::resource_system& resources, const desc::desc_system& descriptions, const desc_file_layout& layout, const res::tag& tag);

	// What desc.read gives and desc.write takes back is edt::code_version_of(bytes); one call moves at most this many bytes.
	inline constexpr std::size_t k_desc_file_limit = 1024 * 1024;

	// Whose {guid, path} references are checked. An agent's guid needs a path and, when the asset index holds it, must name
	// the file of that path, also in an object no schema reads (#954). A person's save needs only the form: the writer
	// puts the path where the guid is now.
	enum class reference_rule { agent, person };

	// Every problem of the desc objects under `root` (the file's content): a root that is no object with a `__type`, an
	// unknown `__type`, a `__version` other than the current one, a field the type does not have or a value that does not
	// fit it, found in each object with a `__type` at any depth (a field holding an inline desc, an array of them).
	// One entry per object at most, `json_path` the JSON pointer of the field in `resource`. Nothing is logged. A reference
	// is a res:// string or {guid, path} (EPIC-57 section 4.4); `resources` (may be null) tells a guid no file holds.
	// `rule` says whose content it is (reference_rule).
	struct desc_tree_check
	{
		std::vector<res::diagnostic> problems;
		// Some reference holds a guid (a {guid, path} object with a UUID that is not nil), `__parent` included.
		bool has_guid = false;
	};
	desc_tree_check validate_desc_tree(const desc::desc_system& descs, const boost::json::value& root, std::string_view resource,
		const res::resource_system* resources = nullptr, reference_rule rule = reference_rule::agent);

	// The root "__guid" string of a desc file's bytes, as the file has it (not checked as an ID); empty when it has none or
	// is no JSON object. What a save that replaces the file keeps until the project's migration takes it out (#772).
	std::string desc_root_guid(std::span<const std::byte> bytes);
	// The root __version of a desc file: 1 when it has none, 0 when the bytes are no JSON object or the version is no
	// whole number of at least 1.
	int desc_root_version(std::span<const std::byte> bytes);
}
