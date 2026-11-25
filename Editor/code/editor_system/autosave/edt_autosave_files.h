#pragma once
#include <snk/model_codec/res_tag.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace edt
{
	// Where autosave keeps its files below the cache:// folder of the project (#1186, #1187; format in EPIC-60).
	inline constexpr std::string_view k_autosave_folder = "autosave";
	inline constexpr std::string_view k_autosave_meta_suffix = ".autosave.json";
	inline constexpr std::string_view k_autosave_session_file = "session.json";
	inline constexpr std::string_view k_autosave_lock_file = "session.lock";
	inline constexpr int k_autosave_format = 2;
	// The kinds of document autosave writes (document::kind()); a folder of the session holds the records of one kind.
	inline constexpr std::string_view k_autosave_kind_scene = "scene";
	inline constexpr std::string_view k_autosave_kind_prefab = "prefab";
	inline constexpr std::string_view k_autosave_kind_data = "data";
	inline constexpr std::string_view k_autosave_kind_mesh = "mesh";
	inline constexpr std::string_view k_autosave_kind_shader_source = "shader_source";
	inline constexpr std::array<std::string_view, 5> k_autosave_kinds{ k_autosave_kind_scene, k_autosave_kind_prefab,
		k_autosave_kind_data, k_autosave_kind_mesh, k_autosave_kind_shader_source };
	bool is_autosave_kind(std::string_view kind);
	// A session folder's name: exactly 32 lowercase hex digits.
	bool is_autosave_session_name(std::string_view name);

	// The folder of a session, relative to the cache folder: "autosave/<session>".
	std::string autosave_session_folder(std::string_view session);
	// The autosave of a document, relative to the cache folder: "autosave/<session>/<kind>/<name>", where <name> is the
	// source's path inside res:// (the whole tag for another protocol) as one file name ('/' is %2F, '%' is %25, and so on),
	// "untitled" for a document without a source.
	std::string autosave_file_of(std::string_view session, std::string_view kind, const res::tag& source);
	// The last name of a path written with '/'.
	std::string_view autosave_leaf_of(std::string_view file);
	// The meta file beside it.
	inline std::string autosave_meta_of(std::string_view file) { return std::string(file) + std::string(k_autosave_meta_suffix); }
	// What the meta's "source" says of a document's source: its path inside res://, the whole tag for another protocol,
	// empty for an untitled one.
	std::string autosave_source_text(const res::tag& source);
	// The res:// tag a meta's "source" names, when it is a plain path inside res:// as autosave_source_text writes it (no
	// protocol, no "." or ".." name, no backslash, colon or control character); else none.
	std::optional<res::tag> autosave_source_tag(std::string_view text);

	// The meta file (format 2): what was saved, of which document, when, by which editor run on which machine, and the
	// source file as it was on disk then.
	struct autosave_meta
	{
		int format = k_autosave_format;
		std::string source;
		std::string kind;
		std::uint64_t generation = 0;
		// UTC, ISO-8601 ("2026-11-24T10:15:00Z").
		std::string saved_at;
		// The source file on disk at the time of the write; unset when there was none or it was not read.
		std::optional<std::uint64_t> source_size;
		std::string source_sha256;
		// The editor run that wrote it: the name of its session folder.
		std::string session;
		// The machine that editor ran on (this_host_name).
		std::string host;
		// The payload file this meta was written with: a payload that differs is not this meta's.
		std::uint64_t payload_size = 0;
		std::string payload_sha256;

		bool operator==(const autosave_meta&) const = default;
	};
	std::string autosave_meta_text(const autosave_meta& meta);
	// nullopt for a text that is not a meta of a known format.
	std::optional<autosave_meta> parse_autosave_meta(std::string_view text);
	// A random id of this editor run (128 bits, 32 lowercase hex), made once: the first session id autosave tries.
	const std::string& autosave_session_id();
	// Another random id of the same form (the folder of the first one exists already).
	std::string autosave_new_session_id();
	// This machine's host name as UTF-8, at most 255 bytes; empty when the system gives none.
	std::string this_host_name();

	// session.json of a session folder: whose run it is and whether its session.lock tells if that run is alive.
	struct autosave_session_info
	{
		int format = k_autosave_format;
		std::string session;
		std::string host;
		std::int64_t pid = 0;
		std::string started_at;
		// false: the lock could not be taken where the session lives, so another editor cannot judge it by the lock.
		bool locked = false;

		bool operator==(const autosave_session_info&) const = default;
	};
	std::string autosave_session_text(const autosave_session_info& info);
	std::optional<autosave_session_info> parse_autosave_session(std::string_view text);
	// The current UTC time as autosave_meta::saved_at writes it.
	std::string autosave_time_now();

	// Writes `data` as `file` and then `meta` beside it, below `cache_root`: each into a temp file next to its target and
	// renamed over it, every folder opened without following a link (platform::beneath). Empty on success, else why not.
	// Any thread.
	std::string write_autosave_files(const std::filesystem::path& cache_root, std::string_view file, std::span<const std::byte> data,
		std::string_view meta);
	// Removes the meta and then the file; nothing there is success. Empty on success, else why not; never throws. Any thread.
	std::string remove_autosave_files(const std::filesystem::path& cache_root, std::string_view file);
	// Writes one small file (session.json) the same way. Empty on success, else why not; never throws. Any thread.
	std::string write_autosave_file(const std::filesystem::path& cache_root, std::string_view file, std::string_view text);
	// Removes one file below the cache folder; nothing there is success. Empty on success, else why not; never throws.
	std::string remove_autosave_file(const std::filesystem::path& cache_root, std::string_view file);
	// Removes "autosave/<name>" and everything in it, never following a link. Empty on success, else why not; never throws.
	std::string remove_autosave_tree(const std::filesystem::path& cache_root, std::string_view name);
}
