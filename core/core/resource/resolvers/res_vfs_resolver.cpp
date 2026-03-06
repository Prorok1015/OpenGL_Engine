#include "res_vfs_resolver.h"
#include "logger/engine_log.h"
#include "common.h"
#include "core_thread_utils.hpp"
#include <fstream>
#include <shared_mutex>

res::vfs_resolver::vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points)
	: core::res::res_resolver_interface(prt)
	, watcher(io, [this](fs::path pth) {
			auto otag = path_mapper(pth);
			if (otag.has_value() && m_on_resource_changed) {
				m_on_resource_changed(otag.value());
			}
		})
{
	if (points.empty())
		return;

	for (auto& root : points) {
		watcher.add_path(root);
		entries_stack.push(root);
	}
	
	watcher.start();
	work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(io));
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
	});
	core::thread::set_name(io_thread, "vfs resolver");
}

res::vfs_resolver::~vfs_resolver()
{
	work_guard.reset();
	watcher.stop();
	io.stop();
	if (io_thread.joinable()) {
		io_thread.join();
	}
}

std::optional<res::tag> res::vfs_resolver::path_mapper(const fs::path& path) const
{
	auto stack = entries_stack;
	while(!stack.empty())
	{
		fs::path full_entry = fs::absolute(stack.top());
		fs::path full_path = fs::absolute(path);
		if (full_path.string().find(full_entry.string()) == 0) {
			std::string relative_path = full_path.string().substr(full_entry.string().length());
			return res::tag(protocol, relative_path);
		}

		stack.pop();
	}
	return std::nullopt;
}

core::res::res_resolver_interface::async_raw_data res::vfs_resolver::resolve(const tag& tag) const
{
	{
		std::shared_lock lock(m_pending_mutex);
		auto it = m_pending_writes.find(tag);
		if (it != m_pending_writes.end()) {
			return it->second;
		}
	}

	if (entries_stack.empty()) {
		auto error_sp = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		error_sp->set_error("No entry points defined for VFS resolver");
		return error_sp;
	}

	auto data_sp = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
	auto path = resolve_tag(tag);

	boost::asio::post(io.get_executor(), std::bind(&vfs_resolver::resolfy_pending_writes, this, data_sp, path));
	return data_sp;
}

bool res::vfs_resolver::store(const::res::tag& tag, const std::vector<std::byte>& data)
{
	auto data_ptr = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
	data_ptr->set_ready(std::vector<std::byte>{ data });

	{
		std::unique_lock lock(m_pending_mutex);
		m_pending_writes[tag] = data_ptr;
	}

	if (entries_stack.empty()) {
		if (m_on_resource_changed) {
			m_on_resource_changed(tag);
		}

		return false;
	}

	fs::path target_path = entries_stack.top() / tag.relative();

	boost::asio::post(io, std::bind(&vfs_resolver::save_pending_write, this, tag, target_path, data_ptr));

	return true;
}

bool res::vfs_resolver::exists(const::res::tag& tag) const
{
	if (m_pending_writes.contains(tag)) return true;
	auto stack = entries_stack;
	while (!stack.empty()) {
		fs::path path = stack.top() / std::string{ tag.relative() };

		if (fs::exists(path)) {
			return true;
		}

		stack.pop();
	}
	return false;
}

fs::path res::vfs_resolver::resolve_tag(const tag& tag) const
{
	auto stack = entries_stack;
	while (!stack.empty())
	{
		fs::path path = stack.top() / tag.relative();
		
		if (fs::exists(path)) {
			return path;
		}

		stack.pop();
	}

	return tag.relative();
}

void res::vfs_resolver::save_pending_write(res::tag tag, fs::path target_path, core::res::res_resolver_interface::async_raw_data data_ptr)
{
	try {
		fs::create_directories(target_path.parent_path());

		auto tmp_path = target_path;
		tmp_path.replace_extension(".tmp");

		{
			std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
			if (file.is_open()) {
				file.write(reinterpret_cast<const char*>(data_ptr->data.data()), data_ptr->data.size());
			}
		}
		fs::rename(tmp_path, target_path);

		{
			std::unique_lock lock(m_pending_mutex);
			m_pending_writes.erase(tag);
			egLOG("resource/write", "Stored resource '{}'", tag.view());
		}

	} catch (std::exception& ex) {
		egLOG("resource/error", "Failed to store resource '{0}': {1}", tag.view(), ex.what());
		ASSERT_FAIL("Storing a file failed!");
	}
}

void res::vfs_resolver::resolfy_pending_writes(core::res::res_resolver_interface::async_raw_data data_sp, fs::path path) const
{
	try {
		egLOG("resource/resolver", "loading file '{}'", path.string());

		if (!fs::exists(path)) {
			data_sp->set_error("File does not exist: " + path.string());
			return;
		}

		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			data_sp->set_error("Failed to open file: " + path.string());
			return;
		}

		auto size = file.tellg();
		std::vector<std::byte> data;

		if (size > 0) {
			file.seekg(0, std::ios::beg);
			data.resize(static_cast<size_t>(size));
			if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
				data_sp->set_error("Failed to read data: " + path.string());
				return;
			}
		}

		data_sp->set_ready(std::move(data));

	}
	catch (const std::exception& e) {
		data_sp->set_error(std::string("Internal IO error: ") + e.what());
		egLOG("resource/error", "Critical error loading {}: {}", path.string(), e.what());
	}
	catch (...) {
		data_sp->set_error("Unknown critical error");
	}
}
