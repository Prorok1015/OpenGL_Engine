#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/model_codec/res_desc_binary_codec.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/res_json_body.h>
#include <snk/resource/res_tag.h>

// Large numeric data of a desc (mesh vertices and indices first) kept in a binary file next to it (EPIC-79, #1068).
// Format, naming and write order: docs/epics/desc-driven/EPIC-79.md. What needs no resource_system is in
// <snk/model_codec/res_desc_binary_codec.h> (snk_model_codec, #1077); reading and saving the pair are here.
namespace res
{
	class resource_system;

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

	namespace desc_binary
	{
		static_assert(max_file_bytes == max_geometry_json_bytes);

		// Checks `file` (named `file_name` in messages) and keeps it with the ranges as spans into it: header, layouts,
		// offsets in u64 without overflow, inside the file, not overlapping; SHA-256 of each range when `verify_hash`.
		SNK_API desc_binary_data parse(std::string_view file_name, std::vector<std::byte>&& file,
			std::span<const binary_range> ranges, bool verify_hash);
		// The same over a copy of `file`.
		SNK_API desc_binary_data parse(std::string_view file_name, std::span<const std::byte> file,
			std::span<const binary_range> ranges, bool verify_hash);

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

		// The desc a save is about to replace, as read under that save's lock before the new one is written (#1093).
		struct previous_desc
		{
			enum class origin : std::uint8_t
			{
				// No desc: the save makes a new one.
				none,
				// In the layer a store writes to: taken back by writing `bytes` again.
				top_layer,
				// In a lower layer only: taken back by removing the new one, so the lower one shows again.
				lower_layer,
				// There, but reading it failed: nothing to take back with (`error` says why).
				unreadable,
			};
			origin from = origin::none;
			std::vector<std::byte> bytes;
			std::string error;
		};

		// Reads the desc `desc` before step 2 of a save, under that save's lock.
		SNK_API previous_desc read_previous_desc(resource_system& system, const save_lock& lock, const tag& desc);

		// After a save that failed at its commit, under that save's lock: puts the previous desc back (top layer), or removes
		// the new desc while it still holds `written_desc` (none, lower layer), and only then the staged file while it still
		// holds `written.file`. A previous desc that could not be read is no reason to remove the new one: refused (WARN),
		// the new desc and the staged file stay.
		SNK_API file_operation take_back(resource_system& system, const save_lock& lock, const tag& desc,
			const desc_binary_written& written, const std::vector<std::byte>& written_desc, const previous_desc& previous);

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
