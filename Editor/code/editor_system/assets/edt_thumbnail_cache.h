#pragma once
#include "edt_thumbnail.h"
#include <snk/platform/plt_beneath.h>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace edt
{
	// What a decode of a picture is given back by (thumbnail_disk_cache::load_or_decode): the thumbnail and the stamp of the
	// file as that read saw it.
	using thumbnail_decode_fn = std::function<thumbnail_decoded(const std::filesystem::path& root,
		const std::filesystem::path& relative, std::optional<platform::beneath::file_stamp>& read_stamp)>;

	// Where the thumbnails may be kept between runs: the project's cache folder, only when there is a project and the person
	// trusts it; otherwise nullopt and they stay in memory (what is written there is read back and shown by the editor).
	inline std::optional<std::filesystem::path> thumbnail_cache_location(const std::optional<std::filesystem::path>& project_cache_dir, bool project_trusted)
	{
		return project_trusted ? project_cache_dir : std::nullopt;
	}

	// The thumbnails of the asset browser kept between runs (#1192 PR 2): `<cache root>/thumbnails/<key>.thumb`, small files
	// of at most about 16 KiB. The key is a SHA-256 of the picture's project-relative path and its whole stamp (size, write
	// time, change time, file id, device) as the decode read it, so another version of a file, or another file at the path,
	// is another key and an old entry is never asked for again; the key is also stored in the entry and checked on read.
	// Everything goes through platform::beneath (never a link), a failed read or write is a miss, never a failed thumbnail.
	// Safe on any worker: every call opens its own pinned_root.
	// The key holds the stamp (file id and device included), which is what makes an entry crafted for another file
	// impractical; on a file system that reports file_id and device as 0 only the path, size and times are in it, and the layer
	// the file came from is not. The pixels carry their own SHA-256, so a half-written body never validates.
	// close() ends it: a closed cache reads and writes nothing, so a job that outlives the project or the person's trust
	// (it holds the cache it was started with) cannot write after that.
	class thumbnail_disk_cache
	{
	public:
		struct limits
		{
			std::size_t max_entries = 4096;
			std::uint64_t max_bytes = 64ull * 1024 * 1024;
			// A write counts towards the next pruning; prune_due() says when it is time again.
			std::size_t prune_every_writes = 256;
			// Entries one listing of the folder gives at most; prune() lists again while the folder holds more.
			std::size_t list_limit = 16384;
		};

		// `cache_root`: the project's cache folder (prj::project_context::cache_dir()).
		explicit thumbnail_disk_cache(std::filesystem::path cache_root);
		thumbnail_disk_cache(std::filesystem::path cache_root, limits with);

		// The stamp of the picture, then the entry for it if there is a good one, else `decode`, whose result is written when the
		// file did not change between the two looks. A decode that fails is returned and kept nowhere.
		thumbnail_decoded load_or_decode(const std::filesystem::path& root, const std::filesystem::path& relative,
			const thumbnail_decode_fn& decode) const;

		// Removes the oldest entries beyond the limits and the leftovers of an interrupted write (an hour old); only names this
		// cache makes are touched. Returns what was removed. Meant for the io lane; also ok on a worker.
		std::size_t prune() const;
		// No read and no write from now on (project closed, trust revoked); cannot be undone, make another cache.
		void close() const { m_open = false; }
		bool is_open() const { return m_open; }
		// Enough writes since the last prune() to call it again (also true before the first call).
		bool prune_due() const;

		static std::string key_of(const std::filesystem::path& relative, const platform::beneath::file_stamp& stamp);
		static std::vector<std::byte> encode_entry(const std::string& key, const thumbnail_pixels& pixels);
		// nullopt for anything that is not exactly the entry of `key`.
		static std::optional<thumbnail_pixels> decode_entry(const std::string& key, std::span<const std::byte> bytes);
		static bool is_entry_name(const std::string& name);

		static constexpr std::size_t k_header_bytes = 4 + 4 + 64 + 4 + 4 + 64;
		static constexpr std::size_t k_max_entry_bytes = k_header_bytes + static_cast<std::size_t>(k_thumbnail_side) * k_thumbnail_side * 4;
		// Where this object's temporary names differ from every other object's (another editor, an older cache of the same folder).
		const std::string& tmp_prefix() const { return m_tmp_prefix; }

		std::size_t hits() const { return m_hits; }
		std::size_t writes() const { return m_writes_total; }

	private:
		std::optional<thumbnail_pixels> read_entry(const std::string& key) const;
		void write_entry(const std::string& key, const thumbnail_pixels& pixels) const;

		std::filesystem::path m_root;
		limits m_limits;
		mutable std::atomic<std::size_t> m_writes_since_prune{ 0 };
		mutable std::atomic<bool> m_pruned_once{ false };
		mutable std::atomic<std::size_t> m_writes_total{ 0 };
		mutable std::atomic<std::size_t> m_hits{ 0 };
		mutable std::atomic<std::uint64_t> m_tmp_serial{ 0 };
		mutable std::atomic<bool> m_open{ true };
		std::string m_tmp_prefix;
	};
}
