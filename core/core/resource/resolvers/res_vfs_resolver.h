#pragma once
#include "res_tag.h"
#include "res_resolver_interface.hpp"
#include "res_file_watcher.h"
#include <vector>
#include <string>
#include <filesystem>
#include <cstddef>
#include <optional>
#include <stack>
#include <shared_mutex>

namespace res
{
	namespace io = boost::asio;
	class vfs_resolver : public core::res::res_resolver_interface
	{
	public:
		vfs_resolver(const std::string_view prt)
			: watcher(io), core::res::res_resolver_interface(prt) {}
		vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points);
		~vfs_resolver();

		std::optional<res::tag> path_mapper(const fs::path& path) const;
		virtual core::res::res_resolver_interface::async_raw_data resolve(const res::tag& tag) const override;
		virtual bool store(const ::res::tag& tag, const std::vector<std::byte>& data) override;
		virtual bool exists(const ::res::tag& tag) const override;

	private:
		fs::path resolve_tag(const tag& tag) const;
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
	};
}
