#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/res_json_body.h>
#include <snk/resource/res_tag.h>

// Large numeric data of a desc (mesh vertices and indices first) kept in a binary file next to it (EPIC-79, #1068).
// Format, naming and write order: docs/epics/desc-driven/EPIC-79.md. What needs no resource_system (the layouts and ranges,
// the tags, encode and save_lock) is in snk_model_codec (#1077), the rest in resource_system.
namespace res
{
	class resource_system;

	enum class binary_scalar : std::uint8_t
	{
		u8,
		u32,
		f32,
	};

	// The element of a range: "u8", "u32", "f32", or "u32xN" / "f32xN" with N in 1..32.
	struct binary_layout
	{
		binary_scalar scalar = binary_scalar::u8;
		std::uint8_t count = 1;

		std::size_t element_bytes() const { return (scalar == binary_scalar::u8 ? 1u : 4u) * count; }
	};

	// One run of bytes in the binary file, as the desc records it. `offset` counts from the start of the file.
	struct binary_range
	{
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		// SHA-256 of the range's bytes, 64 lowercase hex digits.
		std::string sha256;
		std::string layout;
	};

	// Bytes to write as one range.
	struct binary_chunk
	{
		std::span<const std::byte> bytes;
		std::string_view layout;
	};

	// Why a read was refused; a caller tells a hash mismatch (a pair caught mid-save) from a broken file by it.
	enum class desc_binary_error : std::uint8_t
	{
		none,
		missing,
		header,
		range,
		layout,
		hash,
		too_large,
		io,
	};

	// Move-only: `ranges` point into `file`, whose buffer a move keeps.
	struct desc_binary_data
	{
		desc_binary_error error = desc_binary_error::none;
		// Names the file and the range.
		std::string message;
		// The whole file as read.
		std::vector<std::byte> file;
		// One per requested range, in the order asked, into `file`.
		std::vector<std::span<const std::byte>> ranges;
		// Read from the staged file a save left behind (it matched the desc, the binary file did not).
		bool from_staged = false;

		desc_binary_data() = default;
		desc_binary_data(desc_binary_data&&) noexcept = default;
		desc_binary_data& operator=(desc_binary_data&&) noexcept = default;
		desc_binary_data(const desc_binary_data&) = delete;
		desc_binary_data& operator=(const desc_binary_data&) = delete;

		explicit operator bool() const { return error == desc_binary_error::none; }
	};

	// What a read may do with a staged file a save left behind.
	enum class desc_binary_recovery : std::uint8_t
	{
		// Read it when it matches the desc and the binary file does not; write nothing.
		use_staged,
		// Save and editor paths only: also commit it, or delete it when stale, under the save lock.
		commit,
	};

	// The step of desc_binary::save that failed: before `desc` the old pair stays readable; at `commit` the new desc is
	// written and needs the staged file beside it until the caller commits or takes the desc back.
	enum class desc_binary_save_step : std::uint8_t
	{
		none,
		staging,
		desc,
		commit,
	};

	struct desc_binary_written
	{
		// For the desc, in the order of the chunks.
		std::vector<binary_range> ranges;
		// The staged file as written: commit_pending commits only these bytes.
		std::vector<std::byte> file;
		// Empty on success.
		std::string error;
		// Set by save() only; write_pending leaves it none.
		desc_binary_save_step failed_step = desc_binary_save_step::none;

		explicit operator bool() const { return error.empty(); }
	};

	SNK_API std::optional<binary_layout> parse_binary_layout(std::string_view text);

	SNK_API boost::json::object to_json(const binary_range& range);
	// nullopt with `error` set when a field is missing, negative, not an integer, or the hash is not 64 hex digits.
	SNK_API std::optional<binary_range> binary_range_from_json(const boost::json::value& value, std::string& error);

	// The values of a range as T, copied (the bytes in a file have no alignment). False when the size is not a multiple.
	template<class T>
	bool copy_binary_values(std::span<const std::byte> bytes, std::vector<T>& out)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		static_assert(std::endian::native == std::endian::little, "the binary file is little-endian");
		if (bytes.size() % sizeof(T) != 0) {
			return false;
		}
		out.resize(bytes.size() / sizeof(T));
		if (!bytes.empty()) {
			std::memcpy(out.data(), bytes.data(), bytes.size());
		}
		return true;
	}

	namespace desc_binary
	{
		inline constexpr std::uint32_t version = 1;
		// "SNKB", then the version as a little-endian u32.
		inline constexpr std::array<char, 4> magic{ 'S', 'N', 'K', 'B' };
		inline constexpr std::size_t header_bytes = 8;
		// The writer starts every range at a multiple of this.
		inline constexpr std::size_t range_alignment = 16;
		// Binary data is denser than its JSON text, so the geometry JSON limit bounds it with room to spare.
		inline constexpr std::uint64_t max_file_bytes = max_geometry_json_bytes;
		// More ranges in one request are refused.
		inline constexpr std::size_t max_ranges = 64;

		// `<name>.<ext>.desc` -> `<name>.<ext>.bin` in the same folder; nullopt for any other name.
		SNK_API std::optional<tag> binary_tag(const tag& desc);
		// `<name>.<ext>.bin.new`: what a save writes before the desc.
		SNK_API std::optional<tag> staged_tag(const tag& desc);

		// The file for `chunks`: header, then each chunk at the next multiple of range_alignment. Fills `ranges`.
		// nullopt with `error` set for more than max_ranges chunks, an invalid layout, a size that is not a multiple of it,
		// or more than max_file_bytes.
		SNK_API std::optional<std::vector<std::byte>> encode(std::span<const binary_chunk> chunks,
			std::vector<binary_range>& ranges, std::string& error);

		// Checks `file` (named `file_name` in messages) and keeps it with the ranges as spans into it: header, layouts,
		// offsets in u64 without overflow, inside the file, not overlapping; SHA-256 of each range when `verify_hash`.
		SNK_API desc_binary_data parse(std::string_view file_name, std::vector<std::byte>&& file,
			std::span<const binary_range> ranges, bool verify_hash);
		// The same over a copy of `file`.
		SNK_API desc_binary_data parse(std::string_view file_name, std::span<const std::byte> file,
			std::span<const binary_range> ranges, bool verify_hash);

		// Serializes in this process a save of `desc` (write_pending, the desc's store, commit_pending) and a committing
		// read; other processes are kept off only by the compare before a remove. Nests on one thread.
		class SNK_API save_lock
		{
		public:
			explicit save_lock(const tag& desc);
			~save_lock();
			save_lock(const save_lock&) = delete;
			save_lock& operator=(const save_lock&) = delete;

			// Whether this lock serializes the saves of `desc` (the lock of its binary file).
			bool guards(const tag& desc) const;

		private:
			std::shared_ptr<std::recursive_mutex> m_mutex;
		};

		// The ranges of the binary file of `desc`, from the desc's layer, refused before reading over `max_bytes`.
		// A staged file left by a save is used when it matches the desc and the binary file does not (EPIC-79 section 5).
		// Hashes are checked over several roots and beside a staged file always; else when `verify_hash` and the root's
		// verify_desc_binary_hash(), unless its desc_binary_hash_cache knows the pair unchanged (EPIC-79 section 7).
		SNK_API desc_binary_data read(resource_system& system, const tag& desc, std::span<const binary_range> ranges,
			bool verify_hash, std::uint64_t max_bytes = max_file_bytes,
			desc_binary_recovery recovery = desc_binary_recovery::use_staged);

		// Step 1 of a save, under `lock`, which the caller holds through step 3: writes the staged file of `desc` and
		// returns the ranges for the desc. Throws std::logic_error for a lock of another desc or a tag outside limit_writes_to.
		SNK_API desc_binary_written write_pending(resource_system& system, const save_lock& lock, const tag& desc,
			std::span<const binary_chunk> chunks);
		// Step 3, under the lock of step 1: the binary file gets `written.file`, refused when the staged file no longer
		// holds it. Throws like write_pending.
		SNK_API file_operation commit_pending(resource_system& system, const save_lock& lock, const tag& desc,
			const desc_binary_written& written);

		// After a save that failed at its commit, under that save's lock: puts `previous_desc` back (nullopt: removes the desc
		// while it still holds `written_desc`), and only then the staged file while it still holds `written.file`.
		SNK_API file_operation take_back(resource_system& system, const save_lock& lock, const tag& desc,
			const desc_binary_written& written, const std::vector<std::byte>& written_desc,
			const std::optional<std::vector<std::byte>>& previous_desc);

		// Step 2 of save(): stores the desc with `ranges`; false stops the save before the commit.
		using desc_writer = std::function<bool(std::span<const binary_range> ranges)>;

		// The whole save under one save_lock: write_pending, `write_desc`, commit_pending (tried twice). `error` names the
		// failed step and `failed_step` says which: before the desc the old pair stays readable, after it (commit) the new
		// desc needs the staged file, which a reader recovers from; the caller completes or takes back the pair under the lock.
		SNK_API desc_binary_written save(resource_system& system, const tag& desc, std::span<const binary_chunk> chunks,
			const desc_writer& write_desc);
	}

	namespace detail
	{
		// For tests: how many ranges desc_binary::parse has hashed in this process.
		SNK_API std::atomic<std::uint64_t>& desc_binary_hashed_ranges();
		// How old (ns) the write and change times of a pair must be for a hash cache to remember or trust it; 2 s.
		// Tests set it to 0: a change time cannot be backdated.
		SNK_API std::atomic<std::int64_t>& desc_binary_settle_ns();
	}
}
