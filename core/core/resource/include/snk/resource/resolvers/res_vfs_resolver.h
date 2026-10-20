#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resolvers/res_resolver_interface.hpp>
#include <snk/resource/resolvers/res_file_watcher.h>
#include <vector>
#include <string>
#include <filesystem>
#include <cstddef>
#include <optional>
#include <stack>
#include <shared_mutex>
#include <unordered_map>
#include <cstdint>
#include <snk/common/snk_api.h>

namespace res
{
	namespace io = boost::asio;
	class SNK_API vfs_resolver : public core::res::res_resolver_interface
	{
	public:
		vfs_resolver(const std::string_view prt)
			: core::res::res_resolver_interface(prt), watcher(io) {}
		vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points);
		~vfs_resolver();

		std::optional<res::tag> path_mapper(const fs::path& path) const;
		virtual core::res::res_resolver_interface::async_raw_data resolve(const res::tag& tag) const override;
		virtual bool store(const ::res::tag& tag, const std::vector<std::byte>& data) override;
		// Only for the memory protocol (scratch data); false for the other protocols (their files stay) and for a tag
		// this resolver does not hold.
		virtual bool erase(const ::res::tag& tag) override;
		virtual bool exists(const ::res::tag& tag) const override;

	private:
		bool is_shadowed(const fs::path& path) const;
		// The only place a tag becomes a file path: nullopt when the tag would leave `root`.
		std::optional<fs::path> safe_path(const fs::path& root, const tag& tag) const;
		std::optional<fs::path> resolve_tag(const tag& tag) const;
		void save_pending_write(res::tag tag, fs::path path, core::res::res_resolver_interface::async_raw_data data);
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
	};
}
