#pragma once
#include <boost/json/object.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace edt {

	// What the decoder process (#647) hands back: a manifest (JSON) and the resources it stored under memory://, as
	// one file in a folder only the editor can write. The reader treats the file as untrusted input: every size is
	// checked against the file and the limits before anything is allocated.
	struct bundle_blob
	{
		std::string tag;	// memory://...
		std::vector<std::byte> bytes;
	};

	struct decode_bundle
	{
		boost::json::object manifest;
		std::vector<bundle_blob> blobs;
	};

	struct bundle_limits
	{
		std::uint64_t max_manifest_bytes = 4ull * 1024ull * 1024ull;
		std::uint64_t max_blob_bytes = 256ull * 1024ull * 1024ull;
		std::uint64_t max_total_bytes = 512ull * 1024ull * 1024ull;
		std::uint64_t max_blobs = 4096;
		std::size_t max_json_depth = 32;
		std::size_t max_tag_bytes = 512;
	};

	struct bundle_read_result
	{
		std::optional<decode_bundle> bundle;
		std::string error;	// set when `bundle` is empty

		bool ok() const { return bundle.has_value(); }
	};

	// The most bytes a bundle under `limits` can take: its head, the manifest and every blob with its tag and sizes.
	std::uint64_t largest_bundle_bytes(const bundle_limits& limits);

	// A tag a bundle may carry: memory://<name>..., no "..", backslash, control character or empty segment.
	bool is_bundle_tag(const std::string& tag, std::size_t max_bytes);

	// Writes the file; false (with `error`) when the folder cannot be written or a tag is refused.
	bool write_bundle(const std::filesystem::path& file, const decode_bundle& bundle, std::string& error);

	// Who may have made the folder the bundle lies in. The other process's run folder may itself be a link or a junction
	// it made (#1099): it is entered below what holds it, never through a link. A folder of this process is taken as
	// it resolves (a confined decoder may not open what holds its run folder).
	enum class bundle_folder { written_by_peer, own };

	// Reads the file under `limits`. Nothing is returned for a file that is truncated, has bytes after the end, a
	// declared size over its limit or over what is left of the file, a repeated or unacceptable tag, or a manifest
	// that is not a JSON object within the depth limit.
	bundle_read_result read_bundle(const std::filesystem::path& file, const bundle_limits& limits = {},
		bundle_folder folder = bundle_folder::written_by_peer);

} // namespace edt
