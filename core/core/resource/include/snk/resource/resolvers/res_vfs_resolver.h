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
#include <snk/common/snk_api.h>

namespace res
{
	namespace detail {
		class pinned_root;
		struct write_hook_access;
		// Where a store's write may be paused by a test hook (res_vfs_test_hook.h, base only): after the path was checked
		// again on the io thread, and with the temp file written, before the rename.
		enum class write_stage : int { after_check, folders_held, before_rename };
	}

	namespace io = boost::asio;
	class SNK_API vfs_resolver : public core::res::res_resolver_interface
	{
	public:
		explicit vfs_resolver(const std::string_view prt);
		// `policy` limits which paths store() may write (checked against the tag and against the file it resolves to);
		// the default allows every path.
		// `watch` says what the watcher reports and skips (changes_callback only with watch.report_changes).
		vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points, mount_access access = mount_access::read_write,
			write_policy policy = write_policy::allow_all(), watch_options watch = {});
		~vfs_resolver();

		std::optional<res::tag> path_mapper(const fs::path& path) const;
		virtual core::res::res_resolver_interface::async_raw_data resolve(const res::tag& tag) const override;
		virtual bool store(const ::res::tag& tag, const std::vector<std::byte>& data) override;
		// Waits for the io thread to write the file (behind the stores queued before it); false when it was refused or
		// the write failed, and then the file on disk is the one that was there before.
		virtual bool store_sync(const ::res::tag& tag, const std::vector<std::byte>& data) override;
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

	private:
		bool is_shadowed(const fs::path& path) const;
		bool exists_below_top(const tag& tag) const;
		// The only place a tag becomes a file path: nullopt when the tag would leave `root`.
		std::optional<fs::path> safe_path(const fs::path& root, const tag& tag) const;
		std::optional<fs::path> resolve_tag(const tag& tag) const;
		// The file a remove or rename may touch: inside the top root and under the write policy as a tag and as the file
		// it resolves to. nullopt with `why` set otherwise.
		std::optional<fs::path> writable_path(const tag& tag, std::string& why) const;
		// On the io thread; the error text, empty on success.
		std::string remove_on_io(const tag& tag, const fs::path& target);
		std::string rename_on_io(const tag& from, const fs::path& source, const tag& to, const fs::path& target);
		// Tells the changes callback, if the mount reports changes, about what was done through this resolver.
		void report_self_change(const tag& tag, const tag& old_tag, change_kind kind);
		// The file a store writes first and then renames over the target: a tag of its own, so safe_path checks it too.
		// Hidden and unique per store, so it never lands on a real resource or on a link planted at a guessed name.
		static res::tag make_temp_tag(const tag& tag);
		bool queue_store(const ::res::tag& tag, const std::vector<std::byte>& data, std::shared_ptr<std::promise<bool>> done);
		// On the io thread. On any failure the target is left as it was, the temp file is removed and the pending entry
		// dropped, so resolve() does not serve bytes that never reached the disk.
		bool write_pending(const res::tag& tag, const fs::path& target_path, const res::tag& tmp_tag, const fs::path& tmp_path,
			const core::res::res_resolver_interface::async_raw_data& data);
		void resolfy_pending_writes(core::res::res_resolver_interface::async_raw_data data_sp, fs::path path) const;

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
		// Set first in the destructor: a watcher poll still queued must not signal a resource_system being destroyed.
		std::atomic<bool> m_closing{ false };
		// The top root, where stores write (#414): opened on the io thread at the first write, used only there.
		std::unique_ptr<detail::pinned_root> m_write_root;
		// Tests only (detail::write_hook_access): set and cleared while no store is in flight, read on the io thread.
		std::function<void(detail::write_stage, const fs::path& tmp_path)> m_write_hook;
		// Stores queued and not finished: the hook may change only while none is.
		std::atomic<int> m_writes_in_flight{ 0 };
		friend struct detail::write_hook_access;
	};
}
