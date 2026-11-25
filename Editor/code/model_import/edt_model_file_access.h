#pragma once
#include "edt_decoder_sandbox.h"
#include "edt_file_identity.h"
#include "edt_model_import_limits.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace edt {

	enum class model_file_kind { texture, side_file };

	struct referenced_file
	{
		std::filesystem::path path;	// canonical
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id{};
		// Set by the scan from the bytes it read; the import that follows must read the same bytes.
		content_digest digest{};
		bool hashed = false;
		// Every name the model asked for this file by (the scan records them): the import hands the decoder process its
		// bytes under each of them (#1043).
		std::vector<std::string> names{};
		model_file_kind kind = model_file_kind::texture;
	};

	// A file the editor read for the decoder process (#1043): the name the model asked for it by, its name below the
	// boundary (the importer names its textures by it), and its bytes; no bytes: the file is not on the disk.
	struct provided_file
	{
		std::string name{};
		std::string relative{};
		model_file_kind kind = model_file_kind::texture;
		std::shared_ptr<const std::vector<std::byte>> bytes{};
	};

	// A name the model asked for that it was not given (the decoder's scan reports them to the editor, #1043).
	struct requested_file
	{
		std::string name{};
		model_file_kind kind = model_file_kind::texture;
	};

	// What the decoder's scan asks the editor for; `error` set: the model cannot be read, and why.
	struct scan_requests
	{
		std::vector<requested_file> requested{};
		std::string error{};
	};

	struct model_file_limits
	{
		std::uintmax_t max_total_bytes = k_max_side_files_total_bytes;
		std::uintmax_t max_file_bytes = k_max_side_file_bytes;
		std::size_t max_files = k_max_side_files;
	};

	struct model_file_scan
	{
		std::vector<referenced_file> files;
		std::uintmax_t total_bytes = 0;
		std::string error;
		// What the decoder process said of its OS sandbox for this scan (#1076); not reported by an importer in this process.
		decoder_sandbox_status sandbox{};
		bool ok() const { return error.empty(); }
	};

	// Why a side file at a path (lexically normal, as the model names it, never resolved) may not be read; empty when it
	// may. Asked by resolve() before anything of the path is looked at on the disk.
	using model_read_rule = std::function<std::string(const std::filesystem::path&)>;

	// Why resolve() refused a name. `rule`: the name may never be read (outside the folder, hidden, a kind of file or a
	// spelling the rule does not allow, a model that is no file on the disk); `limit`: a count or size limit; `other`:
	// the file on the disk cannot be trusted (not a plain file, its size, time or identity unreadable).
	enum class refusal_kind { none, rule, limit, other };

	struct resolved_reference
	{
		std::filesystem::path path;
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id{};
		model_file_kind kind = model_file_kind::texture;
		std::string error;	// set: refused, and why
		refusal_kind refusal = refusal_kind::none;
		bool found() const { return !path.empty() && error.empty(); }
	};

	// What a model file may make the editor read from the disk for an agent's import: its own folder and the folders
	// below it that are not hidden; images (checked by their first bytes) for textures, a .bin beside a .gltf and a .mtl
	// beside an .obj; a bounded number of files and bytes. Without `allowed` it only collects what the model refers to
	// (the scan the person is shown); with `allowed` it refuses everything the person was not shown, or whose size or
	// modification time changed. `read` checks again what it read, down to its bytes.
	// `boundary` (a folder that holds the model's folder) widens "its own folder" to that folder: a res:// model of the
	// project may read side files anywhere in its content root (#925). Empty, or not around the model: the model's folder.
	// Given `provided` files (the decoder process, #1043), it never touches the disk: a name resolves only to a file the
	// editor read and handed over under that exact name.
	class model_file_access
	{
	public:
		// `record`: a name the model asks for that it was not given is noted in requested() (the scan's rounds).
		// `strict`: such a file is missing, and nothing is read for it (the import).
		enum class provided_mode { record, strict };

		// `on_disk` false: the model was only given as bytes (served from memory or a pack, #925). It has no folder, so every
		// file it refers to is refused and nothing is read from the disk. `may_read` (an agent's import, #1408): a
		// non-empty answer refuses the file by the rule before the disk is asked about it.
		model_file_access(std::filesystem::path root_file, model_file_limits limits, const std::vector<referenced_file>* allowed = nullptr,
			const std::filesystem::path& boundary = {}, bool on_disk = true, model_read_rule may_read = {});
		// `provided` must outlive the access.
		model_file_access(std::filesystem::path root_file, model_file_limits limits, const std::vector<provided_file>* provided, provided_mode mode);

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
		bool on_disk() const { return m_on_disk; }
		bool provided() const { return m_provided != nullptr; }
		const std::vector<requested_file>& requested() const { return m_requested; }

	private:
		resolved_reference resolve_provided(std::string_view reference, model_file_kind kind);
		const provided_file* provided_entry(const resolved_reference& file) const;

		std::filesystem::path m_root_file;
		std::filesystem::path m_dir;
		// The model's folder as it was given, not resolved: an absolute name is checked against it before the disk is asked.
		std::filesystem::path m_dir_as_given;
		// m_dir, or the folder around it the import may read from.
		std::filesystem::path m_boundary;
		std::string m_root_extension;
		model_file_limits m_limits;
		const std::vector<referenced_file>* m_allowed = nullptr;
		const std::vector<provided_file>* m_provided = nullptr;
		model_read_rule m_may_read;
		provided_mode m_mode = provided_mode::strict;
		std::vector<requested_file> m_requested;
		bool m_on_disk = true;
		std::vector<referenced_file> m_files;
		std::unordered_set<std::string> m_seen;
		std::uintmax_t m_listed_bytes = 0;
		std::uintmax_t m_bytes_read = 0;
	};

} // namespace edt
