#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resolvers/res_resolver_interface.hpp>
#include <snk/resource/resolvers/res_file_watcher.h>
#include <snk/resource/resolvers/res_write_policy.h>
#include <snk/resource/res_mount_table.h>
#include <vector>
#include <string>
#include <filesystem>
#include <cstddef>
#include <optional>
#include <stack>
#include <shared_mutex>
#include <unordered_map>
#include <cstdint>
#include <atomic>
#include <future>
#include <functional>
#include <memory>
#include <utility>
#include <snk/common/snk_api.h>
#include <snk/platform/plt_beneath.h>

namespace res
{
	namespace detail {
		struct write_hook_access;
		struct read_hook_access;
		// Where a store's write or a read may be paused by a test hook (res_vfs_test_hook.h, base only); the stages live
		// with the no-follow file primitives in platform::beneath since #523.
		using write_stage = ::platform::beneath::write_stage;
		using read_stage = ::platform::beneath::read_stage;
	}

	namespace io = boost::asio;
	class SNK_API vfs_resolver : public core::res::res_resolver_interface
	{
	public:
		explicit vfs_resolver(const std::string_view prt);
		// `policy` limits which paths store() may write (checked against the tag and against the file it resolves to);
		// the default allows every path.
		// `watch` says what the watcher reports and skips (changes_callback only with watch.report_changes).
		// `watch_files` false: the watcher never starts, so no change on disk is signalled.
		vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points, mount_access access = mount_access::read_write,
			write_policy policy = write_policy::allow_all(), watch_options watch = {}, bool file_operations = false,
			bool watch_files = true);
		~vfs_resolver();

		std::optional<res::tag> path_mapper(const fs::path& path) const;
		virtual core::res::res_resolver_interface::async_raw_data resolve(const res::tag& tag) const override;
		virtual bool store(const ::res::tag& tag, const std::vector<std::byte>& data) override;
		// Waits for the io thread to write the file (behind the stores queued before it); false when it was refused or
		// the write failed, and then the file on disk is the one that was there before.
		virtual bool store_sync(const ::res::tag& tag, const std::vector<std::byte>& data) override;
		// A store that never replaces: the file is made only if nothing is there, also against a file that appears while
		// the data is written (an IDE saving the same name). Returns once it is on disk.
		virtual ::res::file_operation create_sync(const ::res::tag& tag, const std::vector<std::byte>& data) override;
		// Only for the memory protocol (scratch data); false for the other protocols (their files stay) and for a tag
		// this resolver does not hold.
		virtual bool erase(const ::res::tag& tag) override;
		// A regular file of the top root (never a folder or a link), through the opened folders like a store (#414), and
		// under the write policy. Return once the io thread has done it, behind the stores queued before.
		virtual ::res::file_operation remove(const ::res::tag& tag) override;
		// Both tags in this protocol and both under the write policy; refused when `to` exists. Missing folders of `to`
		// are made.
		virtual ::res::file_operation rename(const ::res::tag& from, const ::res::tag& to) override;
		virtual bool exists(const ::res::tag& tag) const override;

		// The file of `tag` from the layer that holds `anchor` (the top layer when none does), read on the calling thread:
		// a file paired with another (a desc's binary file, #1068) never comes from a layer the other is not in.
		::platform::beneath::read_result read_beside(const ::res::tag& anchor, const ::res::tag& tag, std::uint64_t max_bytes,
			std::vector<std::byte>& out) const;
		// The stamp of the file read_beside would read, without reading it; nullopt while a store of it is queued, when it
		// was erased, or when it cannot be opened as read_beside opens it.
		std::optional<::platform::beneath::file_stamp> stamp_beside(const ::res::tag& anchor, const ::res::tag& tag) const;
		// Whether the file of `tag` is in the layer read_beside would read it from; nothing is read.
		bool exists_beside(const ::res::tag& anchor, const ::res::tag& tag) const;
		// The file resolve() opens for `tag` now: the highest layer that has it, the top one when none does (#1086).
		// nullopt while a store of it is queued or it was erased (those bytes are no file), and when every layer refuses
		// the tag.
		std::optional<::res::disk_file> disk_file_of(const ::res::tag& tag) const;
		// disk_file_of, when that is a regular file holding exactly `bytes`. `differs` says why a file that is there is
		// not taken: another size, other bytes, or it cannot be read below its root (a link).
		std::optional<::res::disk_file> disk_file_holding(const ::res::tag& tag, const std::vector<std::byte>& bytes,
			std::string& differs) const;
		// The files below `folder` (relative, '/'-separated) in every layer, as resolve() would find them: links are not
		// followed, a queued store counts, an erased file does not. Sorted, each once.
		std::vector<::res::tag> files_under(std::string_view folder) const;
		// How many folders are mounted: with more than one, the layer of a file depends on which of them have it.
		std::size_t root_count() const;
		// Whether `anchor` resolves to the top layer of a writable mount: the layer a store of its pair writes to.
		bool writes_beside(const ::res::tag& anchor) const;
		// The last step of a two-file save: `target` gets `bytes`, then `staged` is removed only if it still holds them.
		// Refused, writing nothing, when `staged` does not hold them first. Top root, write policy, no file_operations.
		::res::file_operation commit_staged(const ::res::tag& staged, const ::res::tag& target, const std::vector<std::byte>& bytes);
		// Removes the top-root file of `tag` only if it holds exactly `bytes`, compared on the io thread right before.
		::res::file_operation remove_if_same(const ::res::tag& tag, const std::vector<std::byte>& bytes);

	private:
		bool is_shadowed(const fs::path& path) const;
		bool exists_below_top(const tag& tag) const;
		// The only place a tag becomes a file path: nullopt when the tag would leave `root`.
		std::optional<fs::path> safe_path(const fs::path& root, const tag& tag) const;
		// The layer that has the file and its path there (the top layer when none has it); nullopt when every layer
		// rejects the tag.
		std::optional<std::pair<fs::path, fs::path>> resolve_tag(const tag& tag) const;
		// The layer read_beside reads the pair of `anchor` from: the top one while a store of `anchor` is queued.
		std::optional<fs::path> beside_layer(const tag& anchor, std::string& error) const;
		// Whether the top-root file of `tag` (at `path`) holds exactly `bytes`; a queued store of it is what it will hold.
		bool holds_exactly(const tag& tag, const fs::path& path, const std::vector<std::byte>& bytes, std::string& why) const;
		// On the io thread: remove_on_io when the file still holds `bytes`; the error text, empty once removed.
		std::string remove_if_same_on_io(const tag& tag, const fs::path& path, const std::vector<std::byte>& bytes);
		// The file a remove or rename may touch: inside the top root and under the write policy as a tag and as the file
		// it resolves to. nullopt with `why` set otherwise.
		std::optional<fs::path> writable_path(const tag& tag, std::string& why) const;
		// On the io thread; the error text, empty on success.
		std::string remove_on_io(const tag& tag, const fs::path& target);
		std::string rename_on_io(const tag& from, const fs::path& source, const tag& to, const fs::path& target, bool case_only);
		// Tells the changes callback, if the mount reports changes, about what was done through this resolver.
		void report_self_change(const res::tag& changed, const res::tag& old_tag, change_kind kind);
		// The file a store writes first and then renames over the target: a tag of its own, so safe_path checks it too.
		// Hidden and unique per store, so it never lands on a real resource or on a link planted at a guessed name.
		static res::tag make_temp_tag(const tag& tag);
		bool queue_store(const ::res::tag& tag, const std::vector<std::byte>& data, std::shared_ptr<std::promise<bool>> done,
			bool replace = true);
		// On the io thread. On any failure the target is left as it was, the temp file is removed and the pending entry
		// dropped, so resolve() does not serve bytes that never reached the disk.
		bool write_pending(const res::tag& tag, const fs::path& target_path, const res::tag& tmp_tag, const fs::path& tmp_path,
			const core::res::res_resolver_interface::async_raw_data& data, bool replace);
		// On the io thread: the file at `path` below the layer `root`, through platform::beneath::read_beneath (#484).
		void read_from_disk(core::res::res_resolver_interface::async_raw_data data_sp, fs::path root, fs::path path) const;

	private:
		mutable io::io_context io;
		std::unique_ptr<io::executor_work_guard<io::io_context::executor_type>> work_guard;
		std::thread io_thread;
		std::stack<fs::path> entries_stack;
		res::file_watcher watcher;
		mutable std::shared_mutex m_pending_mutex;
		std::unordered_map<res::tag, core::res::res_resolver_interface::async_raw_data> m_pending_writes;
		// Erased tags whose file the io thread has not removed yet -> generation of the erase; resolve() and exists()
		// treat them as absent. Guarded by m_pending_mutex.
		std::unordered_map<res::tag, std::uint64_t> m_erased;
		std::uint64_t m_erase_generation = 0;
		mount_access m_access = mount_access::read_write;
		write_policy m_write_policy = write_policy::allow_all();
		bool m_file_operations = false;
		// Set first in the destructor: a watcher poll still queued must not signal a resource_system being destroyed.
		std::atomic<bool> m_closing{ false };
		// The top root, where stores write (#414): opened on the io thread at the first write, used only there.
		std::unique_ptr<::platform::beneath::pinned_root> m_write_root;
		// Tests only (detail::write_hook_access): set and cleared while no store is in flight, read on the io thread.
		std::function<void(detail::write_stage, const fs::path& tmp_path)> m_write_hook;
		// Stores queued and not finished: the hook may change only while none is.
		std::atomic<int> m_writes_in_flight{ 0 };
		// Tests only (detail::read_hook_access): called on the io thread at each read_stage of a read; and a smaller cap
		// than res::max_external_file_bytes (0: that one). Set while no read is queued.
		std::function<void(detail::read_stage)> m_read_hook;
		std::uint64_t m_read_limit = 0;
		friend struct detail::write_hook_access;
		friend struct detail::read_hook_access;
	};
}
