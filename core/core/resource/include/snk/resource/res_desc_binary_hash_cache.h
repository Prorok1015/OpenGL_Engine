#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <snk/common/snk_api.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_desc_binary.h>

// Pairs of a desc and its binary file whose SHA-256 were checked, so an unchanged pair is not hashed again
// (EPIC-79 section 7, #1126). Speed only: a pair is trusted again only when every field of its key is the same.
// Apart from res_desc_binary.h, which snk_model_codec compiles without the platform headers.
namespace res
{
	struct desc_binary_pair_key
	{
		// The tag text of the binary file.
		std::string binary;
		platform::beneath::file_stamp binary_stamp;
		platform::beneath::file_stamp desc_stamp;
		// desc_binary_ranges_sha256 of the ranges asked for.
		std::string ranges_sha256;

		bool operator==(const desc_binary_pair_key&) const = default;
	};

	// SHA-256 hex of a canonical text of `ranges` in order: offset, size, layout and sha256 of each.
	SNK_API std::string desc_binary_ranges_sha256(std::span<const binary_range> ranges);

	// Set on the root resource_system (set_desc_binary_hash_cache); called from the io threads.
	class SNK_API desc_binary_hash_cache
	{
	public:
		virtual ~desc_binary_hash_cache() = default;

		virtual bool verified(const desc_binary_pair_key& key) const = 0;
		virtual void remember(const desc_binary_pair_key& key) = 0;
	};

	// The editor's cache, kept in <project_root>/.cache/desc_binary_verified.json: loaded once by the constructor, written
	// by flush(). A file that is not exactly the expected format is ignored whole (one warning) and rewritten.
	class SNK_API file_desc_binary_hash_cache final : public desc_binary_hash_cache
	{
	public:
		static constexpr std::size_t max_entries = 65536;
		// Room for max_entries of long tags; a larger file is ignored.
		static constexpr std::uint64_t max_file_bytes = std::uint64_t{ 32 } * 1024 * 1024;
		static constexpr std::int64_t format_version = 1;

		// Relative to the project root.
		static std::filesystem::path file_path();

		explicit file_desc_binary_hash_cache(std::filesystem::path project_root, std::uint64_t file_limit = max_file_bytes);
		// Flushes; never throws.
		~file_desc_binary_hash_cache() override;
		file_desc_binary_hash_cache(const file_desc_binary_hash_cache&) = delete;
		file_desc_binary_hash_cache& operator=(const file_desc_binary_hash_cache&) = delete;

		bool verified(const desc_binary_pair_key& key) const override;
		// A binary file keeps one entry, its latest; past max_entries the oldest go.
		void remember(const desc_binary_pair_key& key) override;

		// Writes the file when something changed since the last write, without following a link. False (logged) when
		// the write failed; the next remember tries again.
		bool flush();
		bool dirty() const;
		std::size_t size() const;

	private:
		void load(std::uint64_t file_limit);

		std::filesystem::path m_project_root;
		mutable std::mutex m_mutex;
		// Oldest first.
		std::list<desc_binary_pair_key> m_entries;
		std::unordered_map<std::string, std::list<desc_binary_pair_key>::iterator> m_by_binary;
		bool m_dirty = false;
		// Two flushes write one after the other, so an older text never lands last.
		std::mutex m_write_mutex;
	};
}
