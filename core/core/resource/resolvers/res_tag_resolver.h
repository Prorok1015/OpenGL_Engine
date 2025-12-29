#pragma once
#include "res_tag.h"
#include "res_resolver_interface.hpp"
#include "res_file_watcher.h"
#include "logger/engine_log.h"
#include <future>
#include <vector>
#include <string>
#include <filesystem>
#include <cstddef>
#include <optional>

namespace res
{
	class resource_resolver : public core::res::res_resolver_interface
	{
	public:
		resource_resolver(std::vector<std::string> entry_points_)
			: entry_points(std::move(entry_points_))
			, watcher(io, [this](fs::path pth){
					auto otag = path_mapper(pth);
					if (otag.has_value() && m_on_resource_changed) {
						m_on_resource_changed(otag.value());
					}
				})
		{
			for (auto& root : entry_points)
				watcher.add_path(root);
			watcher.start();
			work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
				boost::asio::make_work_guard(io)
			);
			io_thread = std::thread([this]() {
				egLOG("resource/resolver", "io thread started");

				while (true) {
					try {
						io.run();
						break;
					}
					catch (const std::exception& e) {
						egLOG("resource/error", "Exception in IO thread: {}", e.what());
						io.restart();
					}
				}

				egLOG("resource/resolver", "io thread stopped");
				}
			);
		}
		
		~resource_resolver()
		{
			work_guard.reset();
			watcher.stop();
			io.stop();
			if (io_thread.joinable()) {
				io_thread.join();
			}
		}

		std::future<std::vector<std::byte>> operator()(const tag& tag) const;

		std::optional<res::tag> path_mapper(const std::filesystem::path& path) const;

		virtual core::res::res_resolver_interface::async_raw_data resolve(const tag& tag) const;

	private:
		std::filesystem::path resolve_tag(const tag& tag) const;

	private:
		mutable boost::asio::io_context io;
		std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard;
		std::thread io_thread;
		std::vector<std::string> entry_points;
		res::file_watcher watcher;
	};
}

