#pragma once
#include "edt_file_identity.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace edt {

	struct referenced_file
	{
		std::filesystem::path path;	// canonical
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id{};
		// Set by the scan from the bytes it read; the import that follows must read the same bytes.
		content_digest digest{};
		bool hashed = false;
	};

	struct model_file_limits
	{
		std::uintmax_t max_total_bytes = 128ull * 1024ull * 1024ull;
		std::uintmax_t max_file_bytes = 64ull * 1024ull * 1024ull;
		std::size_t max_files = 64;
	};

	struct model_file_scan
	{
		std::vector<referenced_file> files;
		std::uintmax_t total_bytes = 0;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	enum class model_file_kind { texture, side_file };

	struct resolved_reference
	{
		std::filesystem::path path;
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id{};
		model_file_kind kind = model_file_kind::texture;
		std::string error;	// set: refused, and why
		bool found() const { return !path.empty() && error.empty(); }
	};

	// What a model file may make the editor read from the disk for an agent's import: its own folder and the folders
	// below it that are not hidden; images (checked by their first bytes) for textures, a .bin beside a .gltf and a .mtl
	// beside an .obj; a bounded number of files and bytes. Without `allowed` it only collects what the model refers to
	// (the scan the person is shown); with `allowed` it refuses everything the person was not shown, or whose size or
	// modification time changed. `read` checks again what it read, down to its bytes.
	class model_file_access
	{
	public:
		model_file_access(std::filesystem::path root_file, model_file_limits limits, const std::vector<referenced_file>* allowed = nullptr);

		// Nothing found and no error: the file is not there (the importer skips such a texture).
		resolved_reference resolve(std::string_view reference, model_file_kind kind);
		// The bytes of a resolved file; throws std::runtime_error when the import would read more than it was allowed.
		std::optional<std::vector<std::byte>> read(const resolved_reference& file);
		// The scan: reads a listed file and records the SHA-256 of its bytes (once per file) that the import must match.
		void hash_listed(const resolved_reference& file);
		// The name below the model folder of a resolved file: one name for every spelling of the same path.
		std::string relative_name(const resolved_reference& file) const;

		const std::vector<referenced_file>& files() const { return m_files; }
		std::uintmax_t listed_bytes() const { return m_listed_bytes; }
		const model_file_limits& limits() const { return m_limits; }

	private:
		std::filesystem::path m_root_file;
		std::filesystem::path m_dir;
		std::string m_root_extension;
		model_file_limits m_limits;
		const std::vector<referenced_file>* m_allowed;
		std::vector<referenced_file> m_files;
		std::unordered_set<std::string> m_seen;
		std::uintmax_t m_listed_bytes = 0;
		std::uintmax_t m_bytes_read = 0;
	};

} // namespace edt
