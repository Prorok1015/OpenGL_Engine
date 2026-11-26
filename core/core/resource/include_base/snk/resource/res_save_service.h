#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <snk/platform/plt_beneath.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Saves of the player (#1487, EPIC-66, ADR-12): the bytes of a slot are written under user://saves/ in a way that an
// interrupted write cannot damage, and the answer is a value, never an assert or an exception.
//
// Base only (include_base/, not installed in the SDK): save_result holds a std::string and save_options a std::function,
// which may not cross the DLL boundary of a game module by value. #1489 gives a module its access and the types a form
// that may.
namespace res {
	class resource_system;

	// Values are only added at the end.
	enum class save_status : std::uint8_t {
		ok,
		// The slot's name is not a name of a save, user:// is not mounted, or what is on disk at the slot is not a file.
		refused,
		io_error,
		too_large,
		// A newer write of the same slot (of the same user:// folder) had already been put on disk when this one came to
		// run: the slot holds newer bytes.
		superseded,
		// Too many bytes are waiting to be written (k_max_queued_bytes): nothing was queued, try again.
		busy,
	};

	struct save_result {
		save_status status = save_status::ok;
		// Why, when the status is not ok; empty otherwise.
		std::string error;
		// What was written (the bytes given), for ok.
		std::uint64_t bytes = 0;
		// The file the slot had was copied to <slot>.bak.
		bool backup_made = false;
		// For ok: the slot is written, but the copy of the file it replaced could not be made (why); empty otherwise. The
		// backup then still holds the generation before.
		std::string backup_error;
	};

	struct save_options {
		// Not set: a file at the slot that is not empty is a good one. Set: is the file at the slot (the bytes about to
		// become the backup) a valid save? A damaged file never replaces the backup: the backup of the last good save
		// stays, and the damaged file is overwritten. A layer that knows the format (a header with a checksum, #1488)
		// sets it. Called on the io thread; an exception is "not valid".
		std::function<bool(std::span<const std::byte>)> is_valid;
	};

	// Where a test may stop a write, to leave the disk as a crash at that point would.
	enum class save_stage : int { before_read, before_replace, after_replace };

	// The service of the player's saves (not a singleton: built over the task system and the resources). The bytes are
	// opaque to it: what a save holds, and any header around it, is the caller's. Writes run on the io lane, one after
	// the other.
	//
	// A slot is a path inside user://saves/ ('/' separated, portable: see slot_problem). Its files:
	//   <slot>        the save;
	//   <slot>.bak    the save before the last write;
	//   .<slot>.<random>.tmp   the bytes being written, never read as a save; the ones a crash left are removed the first
	//                          time the service writes in their folder (one process writes a profile at a time).
	// The order of a write: (1) the file now at the slot is read, (2) the new bytes go to a unique temp file, flushed,
	// then over the slot by a rename, (3) only then the file that was read is copied to <slot>.bak (also by a temp file
	// and a rename). Whatever stops the write, the slot is the old file or the new one, and <slot>.bak is never a copy
	// of what the slot holds before the new bytes are in place: when the disk is full, the backup keeps the generation
	// before. Nothing is followed through a link.
	class SNK_API save_service
	{
	public:
		static constexpr std::uint64_t k_max_bytes = 64ull * 1024 * 1024;
		// Bytes of the writes queued and not yet done; a write over it is `busy`.
		static constexpr std::uint64_t k_max_queued_bytes = 256ull * 1024 * 1024;
		static constexpr std::size_t k_max_slot_bytes = 200;
		static constexpr std::size_t k_max_name_bytes = 100;

		// What the queued bodies share with the service (the tickets of the slots, the queued bytes): internal.
		struct shared_state;

		save_service(ds::task_system& tasks, resource_system& resources);
		~save_service();
		save_service(const save_service&) = delete;
		save_service& operator=(const save_service&) = delete;

		// Why `slot` is not a name of a save, or empty when it is: not portable (res::portable_path_problem), not valid
		// UTF-8, over k_max_slot_bytes, a part over k_max_name_bytes, a character other than a-z, 0-9, space, '_', '-',
		// '.' and '/', a part starting with '.', or ending in .bak, .tmp or .corrupt (those are the service's own files).
		// Names are lowercase ASCII only: slots are told apart without regard to case, so no name is "Save" and "save" at
		// once on a file system that ignores the case (NTFS, APFS), nor "\xC3\x89" and "\xC3\xA9" (É and é). Pure.
		[[nodiscard]] static std::string slot_problem(std::string_view slot);

		// Main thread (user:// is looked up here: during Play it is the test profile). The task answers with the result as a
		// value; a task cancelled before it started writes nothing. Two writes of one slot of one user:// folder are ordered
		// by their calls: when the newer one has been written, the older one that comes later is `superseded`.
		[[nodiscard]] ds::task_handle<save_result> write(std::string_view slot, std::vector<std::byte> bytes, save_options options = {});

		// Main thread: returns once every write queued so far has finished (cancel them first to make it short). Called
		// before user:// is served from other folders (Play's profile is emptied and swapped), so no write of before lands
		// in the new one.
		void flush();

		// Tests only. Called on the io thread at each stage of a write; an exception stops that write there (the disk stays
		// as it was at that point) and the result is io_error.
		void set_stage_hook_for_tests(std::function<void(save_stage)> hook);
		// Tests only: the bytes that may wait to be written (k_max_queued_bytes by default).
		void set_queue_limit_for_tests(std::uint64_t bytes);
		// Tests only. Handed to every file the service writes (the slot, then its backup): called with the temp file written
		// and flushed, just before its rename; an exception stops that write there.
		void set_write_hook_for_tests(platform::beneath::write_hook hook);

	private:
		save_result write_now(const std::filesystem::path& root, const std::string& slot, const std::string& key, std::uint64_t ticket,
			std::span<const std::byte> bytes, const save_options& options);
		void sweep_stale_temp_files(platform::beneath::pinned_root& pinned, const std::filesystem::path& folder);

		ds::task_system& m_tasks;
		resource_system& m_resources;
		// Shared with the queued bodies, which outlive a service whose tasks were cancelled: the tickets and the queued bytes.
		std::shared_ptr<shared_state> m_state;
		// Held for the disk work of one write: the writes of the service run one after the other. Guards the members below.
		std::mutex m_io_mutex;
		std::set<std::string> m_swept;
		std::function<void(save_stage)> m_stage_hook;
		platform::beneath::write_hook m_write_hook;
		// After every member the bodies touch: destroyed first.
		ds::lifetime m_life;
	};
}
