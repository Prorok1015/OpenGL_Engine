#pragma once
#include <snk/model_codec/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace edt
{
	// Where autosave keeps its files below the cache:// folder of the project (#1186; format in EPIC-60).
	inline constexpr std::string_view k_autosave_folder = "autosave";
	inline constexpr std::string_view k_autosave_meta_suffix = ".autosave.json";
	inline constexpr int k_autosave_format = 1;

	// The autosave of a document, relative to the cache folder: "autosave/<kind>/<name>", where <name> is the source's path
	// inside res:// (the whole tag for another protocol) as one file name ('/' is %2F, '%' is %25, and so on), "untitled"
	// for a document without a source.
	std::string autosave_file_of(std::string_view kind, const res::tag& source);
	// The meta file beside it.
	inline std::string autosave_meta_of(std::string_view file) { return std::string(file) + std::string(k_autosave_meta_suffix); }
	// What the meta's "source" says of a document's source: its path inside res://, the whole tag for another protocol,
	// empty for an untitled one.
	std::string autosave_source_text(const res::tag& source);

	// The meta file (format 1): what was saved, of which document, when, and the source file as it was on disk then.
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

		bool operator==(const autosave_meta&) const = default;
	};
	std::string autosave_meta_text(const autosave_meta& meta);
	// nullopt for a text that is not a meta of a known format.
	std::optional<autosave_meta> parse_autosave_meta(std::string_view text);
	// The current UTC time as autosave_meta::saved_at writes it.
	std::string autosave_time_now();

	// Writes `data` as `file` and then `meta` beside it, below `cache_root`: each into a temp file next to its target and
	// renamed over it, every folder opened without following a link (platform::beneath). Empty on success, else why not.
	// Any thread.
	std::string write_autosave_files(const std::filesystem::path& cache_root, std::string_view file, std::span<const std::byte> data,
		std::string_view meta);
	// Removes the meta and then the file; nothing there is success. Empty on success, else why not. Any thread.
	std::string remove_autosave_files(const std::filesystem::path& cache_root, std::string_view file);
}
