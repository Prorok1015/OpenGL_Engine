#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Saves of the player (#1487, EPIC-66, ADR-12): the bytes of a slot are written under user://saves/ in a way that an
// interrupted write cannot damage, and the answer is a value, never an assert or an exception.
namespace res {
	class resource_system;

	// Values are only added at the end: a game module reads them.
	enum class save_status : std::uint8_t {
		ok,
		// The slot's name is not a name of a save, user:// is not mounted, or what is on disk at the slot is not a file.
		refused,
		io_error,
		too_large,
		// A newer write of the same slot had already been put on disk when this one came to run: the slot holds newer bytes.
		superseded,
	};

	struct save_result {
		save_status status = save_status::ok;
		// Why, when the status is not ok; empty otherwise.
		std::string error;
		// What was written (the bytes given), for ok.
		std::uint64_t bytes = 0;
		// The slot had a file and its copy was made as <slot>.bak before it was replaced.
		bool backup_made = false;
	};

	struct save_options {
		// Not set: a file at the slot that is not empty is a good one. Set: is the file at the slot (the bytes about to
		// become the backup) a valid save? A damaged file never replaces the backup: the backup of the last good save
		// stays, and the damaged file is overwritten. A layer that knows the format (a header with a checksum, #1488)
		// sets it. Called on the io thread; an exception is "not valid".
		std::function<bool(std::span<const std::byte>)> is_valid;
	};

	// Where a test may stop a write, to leave the disk as a crash at that point would.
	enum class save_stage : int { before_backup, after_backup, before_replace };

	// The service of the player's saves (not a singleton: built over the task system and the resources). The bytes are
	// opaque to it: what a save holds, and any header around it, is the caller's. Writes run on the io lane; the
	// destructor (main thread) waits for those already queued.
	//
	// A slot is a path inside user://saves/ ('/' separated, portable: see slot_problem). Its files:
	//   <slot>        the save;
	//   <slot>.bak    the save before the last write (made by copying, atomically, before the file is replaced);
	//   .<slot>.<random>.tmp   the bytes being written, never read as a save.
	// A write that stops at any point leaves <slot> as it was or the new bytes, and when it did neither, <slot>.bak holds the
	// previous bytes: the order is (1) the backup, (2) a unique temp file, flushed, (3) a rename over <slot>, (4) a flush of
	// the folder. Nothing is followed through a link.
	class SNK_API save_service
	{
	public:
		static constexpr std::uint64_t k_max_bytes = 64ull * 1024 * 1024;
		static constexpr std::size_t k_max_slot_bytes = 200;

		save_service(ds::task_system& tasks, resource_system& resources);
		~save_service();
		save_service(const save_service&) = delete;
		save_service& operator=(const save_service&) = delete;

		// Why `slot` is not a name of a save, or empty when it is: not portable (res::portable_path_problem), over
		// k_max_slot_bytes, a backslash, a name starting with '.', or ending in .bak, .tmp or .corrupt (those are the
		// service's own files). Pure.
		[[nodiscard]] static std::string slot_problem(std::string_view slot);

		// Main thread (user:// is looked up here: during Play it is the test profile). The task answers with the result as a
		// value; a task cancelled before it started writes nothing. Two writes of one slot are ordered by their calls:
		// when the newer one has been written, the older one that comes later is `superseded`.
		[[nodiscard]] ds::task_handle<save_result> write(std::string_view slot, std::vector<std::byte> bytes, save_options options = {});

		// Tests only: called on the io thread at each stage of a write; an exception stops that write there (the disk stays
		// as it was at that point) and the result is io_error.
		void set_stage_hook_for_tests(std::function<void(save_stage)> hook);

	private:
		save_result write_now(const std::filesystem::path& root, const std::string& slot, std::uint64_t ticket,
			std::span<const std::byte> bytes, const save_options& options);

		ds::task_system& m_tasks;
		resource_system& m_resources;
		std::mutex m_ticket_mutex;
		std::map<std::string, std::uint64_t> m_issued;
		std::map<std::string, std::uint64_t> m_applied;
		// Held for the disk work of one write: the writes of the service run one after the other.
		std::mutex m_io_mutex;
		std::function<void(save_stage)> m_stage_hook;
		// After every member the bodies touch: destroyed first.
		ds::lifetime m_life;
	};
}
