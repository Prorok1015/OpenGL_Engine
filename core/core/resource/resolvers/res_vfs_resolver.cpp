#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/common.h>
#include <snk/common/fs_paths.h>
#include <snk/common/core_thread_utils.hpp>
#include <snk/resource/res_path_sanitize.h>
#include "res_vfs_write.h"
#include <cctype>
#include <format>
#include <fstream>
#include <random>
#include <shared_mutex>
#include <chrono>
#include <thread>
#include <utility>

namespace {

	std::string folded(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

}

res::vfs_resolver::vfs_resolver(const std::string_view prt)
	: core::res::res_resolver_interface(prt), watcher(io)
{
}

res::vfs_resolver::vfs_resolver(const std::string_view prt, const std::vector<fs::path>& points, mount_access access, write_policy policy,
	watch_options watch, bool file_operations)
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
	, m_write_policy(std::move(policy))
	, m_file_operations(file_operations)
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
	// Opened on the first write: the root may not exist yet (a user folder, a project made after the mount).
	m_write_root = std::make_unique<detail::pinned_root>(entries_stack.top());

	if (watch.report_changes) {
		watcher.report_changes(std::move(watch), [this](std::vector<raw_change> found) {
			if (m_closing) {
				return;
			}
			std::vector<file_change> batch;
			batch.reserve(found.size());
			for (const auto& change : found) {
				const auto changed = path_mapper(change.path);
				if (!changed) {
					continue;
				}
				const auto old = change.old_path.empty() ? std::nullopt : path_mapper(change.old_path);
				batch.push_back(file_change{ *changed, old.value_or(res::tag{}), change.kind, change.is_directory, false });
				// What was cached under a file that is gone, moved or newly there is stale; created and modified files are
				// signalled by the watcher's own callback.
				if (!change.is_directory) {
					if (change.kind == change_kind::deleted) {
						notify_resource_changed(*changed);
					}
					else if (change.kind == change_kind::renamed) {
						notify_resource_changed(*changed);
						if (old) {
							notify_resource_changed(*old);
						}
					}
				}
			}
			notify_changes(std::move(batch));
		});
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
			std::string result_str = (rel_path == ".") ? "" : fs_paths::generic_utf8_of(rel_path);

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
	ASSERT_MSG(std::this_thread::get_id() != io_thread.get_id(), "store_sync on the io thread would wait for itself");
	auto done = std::make_shared<std::promise<bool>>();
	auto written = done->get_future();
	if (!queue_store(tag, data, done)) {
		return false;
	}
	return written.get();
}

res::file_operation res::vfs_resolver::create_sync(const::res::tag& tag, const std::vector<std::byte>& data)
{
	ASSERT_MSG(std::this_thread::get_id() != io_thread.get_id(), "create_sync on the io thread would wait for itself");
	if (exists(tag)) {
		return file_operation::failure(std::format("'{}' already exists", tag.view()));
	}
	auto done = std::make_shared<std::promise<bool>>();
	auto written = done->get_future();
	if (!queue_store(tag, data, done, false)) {
		return file_operation::failure(std::format("'{}' may not be written (see the log)", tag.view()));
	}
	if (!written.get()) {
		return file_operation::failure(std::format("'{}' could not be created (it exists now, or the write failed; see the log)", tag.view()));
	}
	return file_operation::success();
}

bool res::vfs_resolver::queue_store(const::res::tag& tag, const std::vector<std::byte>& data,
	std::shared_ptr<std::promise<bool>> done, bool replace)
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
		// Both the tag and the file it resolves to must be writable: a link inside the root may lead to a folder
		// the policy does not cover.
		const std::string tag_path = fs_paths::generic_utf8_of(fs_paths::path_from_utf8(tag.relative()).lexically_normal());
		const std::string file_path = fs_paths::generic_utf8_of(target_path->lexically_relative(entries_stack.top()));
		if (m_file_operations) {
			if (const std::string problem = res::portable_path_problem(tag.relative()); !problem.empty()) {
				egLOG_WARN("resource", "Rejected store of '{}': {}", tag.view(), problem);
				return false;
			}
		}
		if (!m_write_policy.allows(tag_path)) {
			egLOG_WARN("resource", "Rejected store of '{}': no write pattern of protocol '{}' matches '{}'", tag.view(), protocol, tag_path);
			return false;
		}
		if (!m_write_policy.allows(file_path)) {
			egLOG_WARN("resource", "Rejected store of '{}': its file resolves to '{}' below the root (through a link, or a "
				"folder in another case), which no write pattern of protocol '{}' matches", tag.view(), file_path, protocol);
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

	++m_writes_in_flight;
	boost::asio::post(io, [this, tag, target = *target_path, tmp_tag, tmp = *tmp_path, data_ptr, replace, done = std::move(done)] {
		// The promise is fulfilled exactly once, whatever happens: a broken one would throw out of store_sync.
		bool written = false;
		try {
			written = write_pending(tag, target, tmp_tag, tmp, data_ptr, replace);
		} catch (...) {
			written = false;
		}
		// Before the promise: once store_sync returns, no write is in flight.
		--m_writes_in_flight;
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
		// Through the opened folders, like a store: a parent swapped for a link is not followed (#414).
		const fs::path relative = path.lexically_relative(m_write_root->path());
		const std::string error = relative.empty() || *relative.begin() == ".."
			? std::string{ "its file does not lie below the resource root" }
			: detail::remove_beneath(*m_write_root, relative.parent_path(), relative.filename());
		if (!error.empty()) {
			egLOG_WARN("resource/erase", "Failed to remove '{}': {}", path.string(), error);
		}
		std::unique_lock lock(m_pending_mutex);
		if (const auto it = m_erased.find(tag); it != m_erased.end() && it->second == generation) {
			m_erased.erase(it);
		}
	});
	return true;
}

std::optional<fs::path> res::vfs_resolver::writable_path(const tag& tag, std::string& why) const
{
	if (m_access == mount_access::read_only) {
		why = std::format("protocol '{}' is read-only", protocol);
		return std::nullopt;
	}
	if (entries_stack.empty() || !m_write_root) {
		why = std::format("protocol '{}' has no folder mounted", protocol);
		return std::nullopt;
	}
	auto path = safe_path(entries_stack.top(), tag);
	if (!path) {
		why = std::format("'{}' escapes the resource root, or ends in a link", tag.view());
		return std::nullopt;
	}
	if (m_file_operations) {
		why = res::portable_path_problem(tag.relative());
		if (!why.empty()) {
			return std::nullopt;
		}
	}
	const std::string tag_path = fs_paths::generic_utf8_of(fs_paths::path_from_utf8(tag.relative()).lexically_normal());
	const std::string file_path = fs_paths::generic_utf8_of(path->lexically_relative(entries_stack.top()));
	if (!m_write_policy.allows(tag_path)) {
		why = std::format("no write pattern of protocol '{}' matches '{}'", protocol, tag_path);
		return std::nullopt;
	}
	if (!m_write_policy.allows(file_path)) {
		why = std::format("its file resolves to '{}' below the root (through a link, or a folder in another case), which no write "
			"pattern of protocol '{}' matches", file_path, protocol);
		return std::nullopt;
	}
	return path;
}

void res::vfs_resolver::report_self_change(const res::tag& changed, const res::tag& old_tag, change_kind kind)
{
	if (!watcher.reports_changes() || m_closing) {
		return;
	}
	notify_changes({ file_change{ changed, old_tag, kind, false, true } });
}

res::file_operation res::vfs_resolver::remove(const::res::tag& tag)
{
	ASSERT_MSG(std::this_thread::get_id() != io_thread.get_id(), "remove on the io thread would wait for itself");
	if (!m_file_operations) {
		return file_operation::failure(std::format("protocol '{}' does not allow removing files", protocol));
	}
	std::string why;
	const auto target = writable_path(tag, why);
	if (!target) {
		egLOG_WARN("resource/remove", "Rejected remove of '{}': {}", tag.view(), why);
		return file_operation::failure(std::move(why));
	}
	std::error_code ec;
	const auto status = fs::symlink_status(*target, ec);
	if (status.type() == fs::file_type::not_found) {
		return file_operation::failure(std::format("'{}' does not exist", tag.view()));
	}
	if (!fs::is_regular_file(status)) {
		return file_operation::failure(std::format("'{}' is not a regular file", tag.view()));
	}

	auto done = std::make_shared<std::promise<std::string>>();
	auto result = done->get_future();
	boost::asio::post(io, [this, tag, path = *target, done] {
		std::string error;
		try {
			error = remove_on_io(tag, path);
		} catch (const std::exception& ex) {
			error = ex.what();
		}
		done->set_value(std::move(error));
	});
	std::string error = result.get();
	if (!error.empty()) {
		egLOG_WARN("resource/remove", "Failed to remove '{}': {}", tag.view(), error);
		return file_operation::failure(std::move(error));
	}
	return file_operation::success();
}

std::string res::vfs_resolver::remove_on_io(const tag& tag, const fs::path& target)
{
	// Again right before anything is touched, as a store does: a parent may have become a link since remove().
	if (safe_path(entries_stack.top(), tag) != target) {
		return "its file no longer stays inside the resource root";
	}
	const fs::path relative = target.lexically_relative(m_write_root->path());
	if (relative.empty() || *relative.begin() == "..") {
		return "its file does not lie below the resource root";
	}
	if (std::string error = detail::remove_beneath(*m_write_root, relative.parent_path(), relative.filename(), true); !error.empty()) {
		return error;
	}
	{
		std::unique_lock lock(m_pending_mutex);
		m_pending_writes.erase(tag);
	}
	watcher.mark_removed(target);
	report_self_change(tag, res::tag{}, change_kind::deleted);
	egLOG("resource/remove", "Removed resource '{}'", tag.view());
	return {};
}

res::file_operation res::vfs_resolver::rename(const::res::tag& from, const::res::tag& to)
{
	ASSERT_MSG(std::this_thread::get_id() != io_thread.get_id(), "rename on the io thread would wait for itself");
	if (!m_file_operations) {
		return file_operation::failure(std::format("protocol '{}' does not allow renaming files", protocol));
	}
	if (from.protocol() != protocol || to.protocol() != protocol) {
		return file_operation::failure(std::format("'{}' and '{}' are not both in protocol '{}'", from.view(), to.view(), protocol));
	}
	if (from == to) {
		return file_operation::failure("the new name is the old one");
	}
	std::string why;
	const auto source = writable_path(from, why);
	if (!source) {
		egLOG_WARN("resource/rename", "Rejected rename of '{}': {}", from.view(), why);
		return file_operation::failure(std::move(why));
	}
	const auto target = writable_path(to, why);
	if (!target) {
		egLOG_WARN("resource/rename", "Rejected rename to '{}': {}", to.view(), why);
		return file_operation::failure(std::move(why));
	}
	std::error_code ec;
	const auto source_status = fs::symlink_status(*source, ec);
	if (source_status.type() == fs::file_type::not_found) {
		return file_operation::failure(std::format("'{}' does not exist", from.view()));
	}
	if (!fs::is_regular_file(source_status)) {
		return file_operation::failure(std::format("'{}' is not a regular file", from.view()));
	}
	// A target that is the source itself under another case (a.cpp -> A.cpp on Windows or macOS) is a rename, not a clash.
	bool case_only = false;
	if (fs::symlink_status(*target, ec).type() != fs::file_type::not_found) {
		std::error_code same_ec;
		const bool same_file = fs::equivalent(*source, *target, same_ec) && !same_ec;
		case_only = same_file && folded(from.name()) == folded(to.name());
		if (!case_only) {
			return file_operation::failure(std::format("'{}' already exists", to.view()));
		}
	}

	auto done = std::make_shared<std::promise<std::string>>();
	auto result = done->get_future();
	boost::asio::post(io, [this, from, to, source = *source, target = *target, case_only, done] {
		std::string error;
		try {
			error = rename_on_io(from, source, to, target, case_only);
		} catch (const std::exception& ex) {
			error = ex.what();
		}
		done->set_value(std::move(error));
	});
	std::string error = result.get();
	if (!error.empty()) {
		egLOG_WARN("resource/rename", "Failed to rename '{}' to '{}': {}", from.view(), to.view(), error);
		return file_operation::failure(std::move(error));
	}
	return file_operation::success();
}

std::string res::vfs_resolver::rename_on_io(const tag& from, const fs::path& source, const tag& to, const fs::path& target, bool case_only)
{
	if (safe_path(entries_stack.top(), from) != source || safe_path(entries_stack.top(), to) != target) {
		return "its file no longer stays inside the resource root";
	}
	const fs::path from_relative = source.lexically_relative(m_write_root->path());
	const fs::path to_relative = target.lexically_relative(m_write_root->path());
	if (from_relative.empty() || *from_relative.begin() == ".." || to_relative.empty() || *to_relative.begin() == "..") {
		return "its file does not lie below the resource root";
	}
	detail::rename_request request;
	request.from_folder = from_relative.parent_path();
	request.from_leaf = from_relative.filename();
	request.to_folder = to_relative.parent_path();
	request.to_leaf = to_relative.filename();
	request.allow_replace = case_only;
	if (std::string error = detail::rename_beneath(*m_write_root, request); !error.empty()) {
		return error;
	}
	{
		std::unique_lock lock(m_pending_mutex);
		m_pending_writes.erase(from);
		m_pending_writes.erase(to);
	}
	watcher.mark_renamed(source, target);
	report_self_change(to, from, change_kind::renamed);
	egLOG("resource/rename", "Renamed resource '{}' to '{}'", from.view(), to.view());
	return {};
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
	const fs::path rel = fs_paths::path_from_utf8(rel_view);

	// An empty tag would map to the root itself; control characters have no business in a file name.
	if (rel_view.empty() || res::has_control_chars(rel_view)) {
		return std::nullopt;
	}
	if (rel.has_root_name() || rel.has_root_directory()) {
		return std::nullopt;
	}
	for (const auto& part : rel) {
		const std::string name = fs_paths::generic_utf8_of(part);
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
	const fs::path& tmp_path, const core::res::res_resolver_interface::async_raw_data& data_ptr, bool replace)
{
	// The temp file is never removed by path here: write_beneath removes its own, inside the folder it opened.
	const auto fail = [&](std::string_view why) {
		egLOG_ERROR("resource/write", "Failed to store '{}': {}", tag.view(), why);
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
			return fail("its file or temp file no longer stays inside the resource root");
		}

		if (m_write_hook) {
			m_write_hook(detail::write_stage::after_check, tmp_path);
		}

		// From here on nothing goes by path: the folders below the root are opened one by one without following a
		// link, so one swapped for a link since the check above is refused instead of followed (#414).
		const fs::path relative = target_path.lexically_relative(m_write_root->path());
		if (relative.empty() || *relative.begin() == ".." || tmp_path.parent_path() != target_path.parent_path()) {
			return fail("its file does not lie below the resource root");
		}
		std::error_code exists_ec;
		const bool existed = fs::exists(target_path, exists_ec);
		detail::write_request request;
		request.folder = relative.parent_path();
		request.leaf = relative.filename();
		request.tmp_leaf = tmp_path.filename();
		request.data = data_ptr->data;
		// Scratch data of the process: not worth an fsync per store (#417).
		request.durable = protocol != ::res::tag::memory;
		request.replace = replace;
		request.hook = &m_write_hook;
		request.tmp_path = tmp_path;
		if (const std::string error = detail::write_beneath(*m_write_root, request); !error.empty()) {
			return fail(error);
		}

		// Update file_watcher snapshot so it doesn't treat our own write as an external change.
		watcher.mark_written(target_path);
		report_self_change(tag, res::tag{}, existed ? change_kind::modified : change_kind::created);

		{
			std::unique_lock lock(m_pending_mutex);
			if (auto it = m_pending_writes.find(tag); it != m_pending_writes.end() && it->second == data_ptr) {
				m_pending_writes.erase(it);
			}
		}
		egLOG("resource/write", "Stored resource '{}'", tag.view());
		return true;
	} catch (const std::exception& ex) {
		return fail(ex.what());
	}
}

void res::vfs_resolver::resolfy_pending_writes(core::res::res_resolver_interface::async_raw_data data_sp, fs::path path) const
{
	try {
		egLOG("resource/resolver", "loading file '{}'", path.string());

		std::error_code ec;
		const auto status = fs::status(path, ec);
		if (status.type() == fs::file_type::not_found) {
			data_sp->set_error("File does not exist: " + path.string());
			return;
		}
		if (ec) {
			data_sp->set_error("Cannot access '" + path.string() + "': " + ec.message());
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

		// The file may have been swapped since status(): a size the file system does not confirm is not allocated.
		auto size = file.tellg();
		const auto file_bytes = fs::file_size(path, ec);
		if (size < 0 || ec || static_cast<std::uintmax_t>(size) > file_bytes) {
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
