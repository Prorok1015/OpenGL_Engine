#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/common.h>
#include <snk/common/core_thread_utils.hpp>
#include <snk/resource/res_path_sanitize.h>
#include <cctype>
#include <format>
#include <fstream>
#include <random>
#include <shared_mutex>
#include <chrono>
#include <thread>

res::vfs_resolver::vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points, mount_access access)
	: core::res::res_resolver_interface(prt)
	, watcher(io, [this](fs::path pth) {
			if (m_closing) {
				return;
			}
			auto otag = path_mapper(pth);
			if (otag.has_value() && !is_shadowed(pth)) {
				notify_resource_changed(otag.value());
			}
		})
	, m_access(access)
{
	if (points.empty())
		return;

	for (const auto& root : points) {
		// Pinned once: safe_path checks containment against this value, so a root swapped for a symlink later does
		// not move the mount.
		std::error_code ec;
		fs::path pinned = fs::weakly_canonical(root, ec);
		if (ec) {
			pinned = root;
		}
		watcher.add_path(pinned);
		entries_stack.push(pinned);
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
	// No io.stop(): it would drop the writes still queued, e.g. a level saved right before the editor exits.
	// With the guard gone and the watcher stopped (behind those writes, on the io thread), run() returns once the
	// queue is empty.
	m_closing = true;
	work_guard.reset();
	boost::asio::post(io, [this] { watcher.stop(); });
	if (io_thread.joinable()) {
		io_thread.join();
	}
}

std::optional<res::tag> res::vfs_resolver::path_mapper(const fs::path& path) const
{
	// The mounted roots are canonical, so the path is too: a symlink in either would otherwise break the match.
	std::error_code ec;
	fs::path full_path = fs::weakly_canonical(path, ec);
	if (ec) {
		full_path = fs::absolute(path).lexically_normal();
	}
	auto stack = entries_stack;

	while (!stack.empty()) {
		fs::path full_entry = fs::absolute(stack.top()).lexically_normal();

		fs::path rel_path = full_path.lexically_relative(full_entry);

		if (!rel_path.empty() && *rel_path.begin() != "..") {
			std::string result_str = (rel_path == ".") ? "" : rel_path.generic_string();

			return res::tag(protocol, result_str);
		}

		stack.pop();
	}
	return std::nullopt;
}

bool res::vfs_resolver::is_shadowed(const fs::path& path) const
{
	fs::path full_path = fs::absolute(path).lexically_normal();
	auto stack = entries_stack;
	std::vector<fs::path> higher;

	while (!stack.empty()) {
		fs::path full_entry = fs::absolute(stack.top()).lexically_normal();
		fs::path rel_path = full_path.lexically_relative(full_entry);

		if (!rel_path.empty() && *rel_path.begin() != "..") {
			// A change in a lower layer is invisible while a higher layer has a file at the same path.
			for (const auto& upper : higher) {
				std::error_code ec;
				if (fs::exists(upper / rel_path, ec)) {
					return true;
				}
			}
			return false;
		}
		higher.push_back(full_entry);
		stack.pop();
	}
	return false;
}

core::res::res_resolver_interface::async_raw_data res::vfs_resolver::resolve(const tag& tag) const
{
	{
		std::shared_lock lock(m_pending_mutex);
		auto it = m_pending_writes.find(tag);
		if (it != m_pending_writes.end()) {
			return it->second;
		}
		if (m_erased.contains(tag)) {
			auto erased_sp = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
			erased_sp->set_error("Resource was erased: " + tag.string());
			return erased_sp;
		}
	}

	if (entries_stack.empty()) {
		auto error_sp = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		error_sp->set_error("No entry points defined for VFS resolver");
		return error_sp;
	}

	auto data_sp = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
	auto path = resolve_tag(tag);
	if (!path.has_value()) {
		egLOG_WARN("resource", "Rejected resolve of '{}': path escapes the resource root", tag.view());
		data_sp->set_error("Tag escapes the resource root: " + tag.string());
		return data_sp;
	}

	boost::asio::post(io.get_executor(), std::bind(&vfs_resolver::resolfy_pending_writes, this, data_sp, *path));
	return data_sp;
}

bool res::vfs_resolver::store(const::res::tag& tag, const std::vector<std::byte>& data)
{
	return queue_store(tag, data, nullptr);
}

bool res::vfs_resolver::store_sync(const::res::tag& tag, const std::vector<std::byte>& data)
{
	auto done = std::make_shared<std::promise<bool>>();
	auto written = done->get_future();
	if (!queue_store(tag, data, done)) {
		return false;
	}
	return written.get();
}

bool res::vfs_resolver::queue_store(const::res::tag& tag, const std::vector<std::byte>& data,
	std::shared_ptr<std::promise<bool>> done)
{
	if (m_access == mount_access::read_only) {
		egLOG_WARN("resource", "Rejected store of '{}': protocol '{}' is read-only", tag.view(), protocol);
		return false;
	}

	std::optional<fs::path> target_path;
	const res::tag tmp_tag = make_temp_tag(tag);
	std::optional<fs::path> tmp_path;
	if (!entries_stack.empty()) {
		target_path = safe_path(entries_stack.top(), tag);
		tmp_path = safe_path(entries_stack.top(), tmp_tag);
		if (!target_path.has_value() || !tmp_path.has_value()) {
			egLOG_WARN("resource", "Rejected store of '{}': path escapes the resource root", tag.view());
			return false;
		}
		std::error_code ec;
		if (!fs::exists(*target_path, ec) && exists_below_top(tag)) {
			egLOG_WARN("resource", "Store of '{}' creates '{}', which now overrides the file of a lower layer",
				tag.view(), target_path->string());
		}
	}

	auto data_ptr = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
	data_ptr->set_ready(std::vector<std::byte>{ data });

	{
		std::unique_lock lock(m_pending_mutex);
		m_pending_writes[tag] = data_ptr;
		m_erased.erase(tag); // a store after an erase: the pending erase task finds no mark and leaves the file alone
	}

	if (entries_stack.empty()) {
		notify_resource_changed(tag);

		return false;
	}

	boost::asio::post(io, [this, tag, target = *target_path, tmp_tag, tmp = *tmp_path, data_ptr, done = std::move(done)] {
		const bool written = write_pending(tag, target, tmp_tag, tmp, data_ptr);
		if (done) {
			done->set_value(written);
		}
	});

	return true;
}

bool res::vfs_resolver::erase(const::res::tag& tag)
{
	if (protocol != ::res::tag::memory) {
		egLOG_WARN("resource/erase", "Refusing to erase '{}': only the memory protocol can be erased", tag.view());
		return false;
	}

	std::optional<fs::path> target_path;
	if (!entries_stack.empty()) {
		target_path = safe_path(entries_stack.top(), tag);
		if (!target_path.has_value()) {
			egLOG_WARN("resource/erase", "Rejected erase of '{}': path escapes the resource root", tag.view());
			return false;
		}
	}

	if (!exists(tag)) {
		return false;
	}

	std::uint64_t generation = 0;
	{
		std::unique_lock lock(m_pending_mutex);
		m_pending_writes.erase(tag);
		generation = ++m_erase_generation;
		m_erased[tag] = generation;
	}

	if (entries_stack.empty()) {
		return true;
	}

	// On the io thread, behind any write_pending already queued for the tag. The mark holds the generation of the
	// erase that set it: a later store() clears it and a later erase() replaces it, and then this task leaves the file
	// to whoever did that. The mark goes only after the file is gone, so resolve() and exists() never see the old file.
	boost::asio::post(io, [this, tag, generation, path = *target_path]() {
		const auto still_current = [&]() {
			std::shared_lock lock(m_pending_mutex);
			const auto it = m_erased.find(tag);
			return it != m_erased.end() && it->second == generation;
		};
		if (!still_current()) {
			return;
		}
		std::error_code ec;
		fs::remove(path, ec);
		if (ec) {
			egLOG_WARN("resource/erase", "Failed to remove '{}': {}", path.string(), ec.message());
		}
		std::unique_lock lock(m_pending_mutex);
		if (const auto it = m_erased.find(tag); it != m_erased.end() && it->second == generation) {
			m_erased.erase(it);
		}
	});
	return true;
}

bool res::vfs_resolver::exists(const::res::tag& tag) const
{
	{
		std::shared_lock lock(m_pending_mutex);
		if (m_pending_writes.contains(tag)) return true;
		if (m_erased.contains(tag)) return false;
	}
	auto stack = entries_stack;
	while (!stack.empty()) {
		const auto path = safe_path(stack.top(), tag);
		std::error_code ec;
		if (path.has_value() && fs::exists(*path, ec)) {
			return true;
		}

		stack.pop();
	}
	return false;
}

bool res::vfs_resolver::exists_below_top(const tag& tag) const
{
	auto stack = entries_stack;
	if (stack.empty()) {
		return false;
	}
	stack.pop();
	while (!stack.empty()) {
		const auto path = safe_path(stack.top(), tag);
		std::error_code ec;
		if (path.has_value() && fs::exists(*path, ec)) {
			return true;
		}
		stack.pop();
	}
	return false;
}

std::optional<fs::path> res::vfs_resolver::safe_path(const fs::path& root, const tag& tag) const
{
	const std::string_view rel_view = tag.relative();
	const fs::path rel{ rel_view };

	// An empty tag would map to the root itself; control characters have no business in a file name.
	if (rel_view.empty() || res::has_control_chars(rel_view)) {
		return std::nullopt;
	}
	if (rel.has_root_name() || rel.has_root_directory()) {
		return std::nullopt;
	}
	for (const auto& part : rel) {
		const std::string name = part.generic_string();
		if (name == "..") {
			return std::nullopt;
		}
		// A drive letter is not a root name on POSIX, but the same tag must be rejected everywhere.
		if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') {
			return std::nullopt;
		}
	}

	std::error_code ec;
	// The constructor pinned the root canonical; it is not resolved again.
	fs::path canon_root = root;
	// Only the directory is canonicalized: an erase on the io thread may remove the file while
	// weakly_canonical probes it, which fails the whole call and rejected a valid store.
	const fs::path joined = canon_root / rel;
	if (!joined.has_filename()) {
		return std::nullopt;
	}
	fs::path candidate = fs::weakly_canonical(joined.parent_path(), ec);
	if (ec) {
		return std::nullopt;
	}
	candidate /= joined.filename();
	// The last component is not resolved above, so a symlink there could point anywhere.
	if (fs::is_symlink(fs::symlink_status(candidate, ec))) {
		return std::nullopt;
	}

	if (!canon_root.has_filename()) {
		canon_root = canon_root.parent_path();
	}
	// Compare whole components: "/sandbox2" must not count as inside "/sandbox".
	auto cand_it = candidate.begin();
	for (auto root_it = canon_root.begin(); root_it != canon_root.end(); ++root_it, ++cand_it) {
		if (cand_it == candidate.end() || *root_it != *cand_it) {
			return std::nullopt;
		}
	}
	return candidate;
}

std::optional<fs::path> res::vfs_resolver::resolve_tag(const tag& tag) const
{
	auto stack = entries_stack;
	std::optional<fs::path> first_accepted;
	while (!stack.empty())
	{
		auto path = safe_path(stack.top(), tag);
		if (path.has_value()) {
			std::error_code ec;
			if (fs::exists(*path, ec)) {
				return path;
			}
			if (!first_accepted.has_value()) {
				first_accepted = std::move(path);
			}
		}

		stack.pop();
	}

	// Missing everywhere: report the path in the top layer; nullopt when every layer rejected the tag.
	return first_accepted;
}

res::tag res::vfs_resolver::make_temp_tag(const tag& tag)
{
	thread_local std::mt19937_64 random{ std::random_device{}() };
	const std::string_view full = tag.view();
	const std::string_view folder = full.substr(0, full.size() - tag.name().size());
	return res::tag{ std::format("{}.{}.{:016x}.tmp", folder, tag.name(), random()) };
}

bool res::vfs_resolver::write_pending(const res::tag& tag, const fs::path& target_path, const res::tag& tmp_tag,
	const fs::path& tmp_path, const core::res::res_resolver_interface::async_raw_data& data_ptr)
{
	const auto fail = [&](std::string_view why, bool remove_tmp) {
		egLOG_ERROR("resource/write", "Failed to store '{}': {}", tag.view(), why);
		if (remove_tmp) {
			std::error_code ignored;
			fs::remove(tmp_path, ignored);
		}
		// A newer store of the tag keeps its own entry.
		std::unique_lock lock(m_pending_mutex);
		if (auto it = m_pending_writes.find(tag); it != m_pending_writes.end() && it->second == data_ptr) {
			m_pending_writes.erase(it);
		}
		return false;
	};

	try {
		// Again right before anything is created: a parent or a leaf may have become a symlink since store(), and
		// removing the temp file through such a parent could hit a file outside the root.
		if (safe_path(entries_stack.top(), tag) != target_path || safe_path(entries_stack.top(), tmp_tag) != tmp_path) {
			return fail("its file or temp file no longer stays inside the resource root", false);
		}

		std::error_code ec;
		fs::create_directories(target_path.parent_path(), ec);
		if (ec) {
			return fail(std::format("cannot create '{}': {}", target_path.parent_path().string(), ec.message()), false);
		}

		{
			std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				return fail(std::format("cannot open '{}' for writing", tmp_path.string()), true);
			}
			file.write(reinterpret_cast<const char*>(data_ptr->data.data()), static_cast<std::streamsize>(data_ptr->data.size()));
			// close() flushes: a full disk shows up here, and a truncated file must not replace the good one.
			file.close();
			if (file.fail()) {
				return fail(std::format("writing '{}' failed", tmp_path.string()), true);
			}
		}

		// On Windows a file that was just removed (an erase on this thread) can stay delete-pending while
		// another handle (watcher, indexer, antivirus) is open on it, and renaming onto it is denied for a moment.
		std::error_code rename_error;
		for (int attempt = 0; attempt < 50; ++attempt) {
			fs::rename(tmp_path, target_path, rename_error);
			if (!rename_error) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (rename_error) {
			return fail(std::format("cannot replace '{}': {}", target_path.string(), rename_error.message()), true);
		}

		// Update file_watcher snapshot so it doesn't treat our own write as an external change.
		watcher.mark_written(target_path);

		{
			std::unique_lock lock(m_pending_mutex);
			if (auto it = m_pending_writes.find(tag); it != m_pending_writes.end() && it->second == data_ptr) {
				m_pending_writes.erase(it);
			}
		}
		egLOG("resource/write", "Stored resource '{}'", tag.view());
		return true;
	} catch (const std::exception& ex) {
		return fail(ex.what(), true);
	}
}

void res::vfs_resolver::resolfy_pending_writes(core::res::res_resolver_interface::async_raw_data data_sp, fs::path path) const
{
	try {
		egLOG("resource/resolver", "loading file '{}'", path.string());

		std::error_code ec;
		const auto status = fs::status(path, ec);
		if (!fs::exists(status)) {
			data_sp->set_error("File does not exist: " + path.string());
			return;
		}
		// A folder opens as a stream on Linux, and tellg() then reports LLONG_MAX.
		if (!fs::is_regular_file(status)) {
			data_sp->set_error("Not a regular file: " + path.string());
			return;
		}

		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			data_sp->set_error("Failed to open file: " + path.string());
			return;
		}

		auto size = file.tellg();
		if (size < 0) {
			data_sp->set_error("Failed to get the size of: " + path.string());
			return;
		}
		std::vector<std::byte> data;

		if (size > 0) {
			file.seekg(0, std::ios::beg);
			data.resize(static_cast<size_t>(size));
			if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
				data_sp->set_error("Failed to read data: " + path.string());
				return;
			}
		}

		// Close before waking waiters: on Windows an open handle blocks deleting or replacing the file.
		file.close();
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
