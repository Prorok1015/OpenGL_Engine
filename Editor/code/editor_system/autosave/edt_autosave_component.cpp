#include "edt_autosave_component.h"
#include "edt_autosave_files.h"
#include "edt_autosave_source.h"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/common/fs_paths.h>
#include <snk/model_codec/res_tag.h>
#include <snk/platform/plt_beneath.h>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <exception>
#include <mutex>
#include <format>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	// A source this big is not hashed for the meta: the hash only helps crash recovery tell a changed source.
	constexpr std::uintmax_t k_hashed_source_limit = std::uintmax_t{ 256 } << 20;

	// What a write needs, copied on the main thread: it may run on the io lane.
	struct write_order
	{
		fs::path root;
		std::string file;
		edt::autosave_snapshot snapshot;
		edt::autosave_meta meta;
		std::optional<fs::path> source_file;
	};

	std::string write_now(write_order& order)
	{
		const std::vector<std::byte> bytes = edt::autosave_bytes(order.snapshot);
		if (bytes.empty()) {
			return "nothing to write";
		}
		order.meta.payload_size = bytes.size();
		order.meta.payload_sha256 = cmn::sha256_hex(bytes);
		if (order.source_file) {
			std::error_code ec;
			const std::uintmax_t size = fs::file_size(*order.source_file, ec);
			if (!ec && size <= k_hashed_source_limit) {
				if (std::optional<std::string> sha = cmn::sha256_file_hex(*order.source_file)) {
					order.meta.source_size = static_cast<std::uint64_t>(size);
					order.meta.source_sha256 = std::move(*sha);
				}
			}
		}
		order.meta.saved_at = edt::autosave_time_now();
		return edt::write_autosave_files(order.root, order.file, bytes, edt::autosave_meta_text(order.meta));
	}

	std::string run_write(write_order& order) noexcept
	{
		try {
			return write_now(order);
		} catch (const std::exception& error) {
			return error.what();
		} catch (...) {
			return "an unknown error";
		}
	}
}

fs::path edt::autosave_cache_root(const res::resource_system& resources)
{
	const std::vector<fs::path> roots = resources.mount_roots(res::tag::cache);
	return roots.empty() ? fs::path{} : roots.back();
}

edt::autosave_component::autosave_component(ports from)
	: m_ports(std::move(from))
	, m_session(autosave_session_id())
	, m_host(this_host_name())
	, m_clock([] { return clock::now(); })
{
	m_closed = m_ports.events.document_closed.subscribe([this](document_id id) { forget(id); });
}

edt::autosave_component::~autosave_component()
{
	shutdown();
}

std::string edt::autosave_component::written_file(document_id id) const
{
	const auto found = m_tracked.find(id);
	return found != m_tracked.end() && found->second.on_disk ? found->second.disk_file : std::string{};
}

void edt::autosave_component::refresh_root()
{
	const fs::path root = autosave_cache_root(m_ports.resources);
	if (root == m_root) {
		return;
	}
	// Another project: what was written for the old one is no autosave of anything open now.
	for (auto& [id, record] : m_tracked) {
		discard(record);
		record.due.reset();
		record.written_generation.reset();
	}
	end_session();
	m_root = root;
	m_session_failed = false;
	if (m_root.empty() && !m_reported_no_project) {
		m_reported_no_project = true;
		egLOG_INFO("editor/autosave", "no project is open (no cache:// folder): autosave is off");
	}
}

void edt::autosave_component::tick()
{
	if (m_shut) return;
	refresh_root();
	const clock::time_point now = m_clock();
	const bool on = m_settings.enabled && m_allowed;
	if (!on) {
		// Turned off: what it wrote is no autosave the person wants kept.
		for (auto& [id, record] : m_tracked) {
			discard(record);
			record.due.reset();
			record.written_generation.reset();
		}
	}
	const bool can_write = on && !m_root.empty() && ensure_session();
	std::optional<bool> gesture;
	std::vector<document_id> seen;
	seen.reserve(m_tracked.size() + 1);
	m_ports.documents.for_each([&](document& doc) {
		seen.push_back(doc.id());
		follow(doc, now, can_write, gesture);
	});
	std::vector<document_id> gone;
	for (const auto& [id, record] : m_tracked) {
		if (!record.closed && std::find(seen.begin(), seen.end(), id) == seen.end()) gone.push_back(id);
	}
	for (const document_id id : gone) {
		forget(id);
	}
}

void edt::autosave_component::follow(document& doc, clock::time_point now, bool can_write, std::optional<bool>& gesture)
{
	const document_id id = doc.id();
	tracked& record = m_tracked[id];
	if (record.closed) return;
	std::string file = autosave_file_of(m_session, doc.kind(), doc.source());
	if (record.file != file) {
		// Another source (another level, a Save As): the files of the old one go.
		discard(record);
		record.file = std::move(file);
		record.due.reset();
		record.written_generation.reset();
	}
	const std::uint64_t generation = doc.generation();
	if (!doc.is_dirty()) {
		discard(record);
		record.due.reset();
		record.written_generation.reset();
		report(id, {}, {});
		return;
	}
	if (!can_write) {
		return;
	}
	if (!record.due && record.written_generation != generation) {
		record.due = now + std::chrono::seconds(std::clamp(m_settings.interval_seconds, k_autosave_min_interval, k_autosave_max_interval));
	}
	if (!record.due || now < *record.due || record.in_flight || doc.edits_locked()) {
		return;
	}
	if (!gesture) {
		gesture = m_ports.gesture_active && m_ports.gesture_active();
	}
	if (*gesture) {
		return;
	}
	write(doc, record, id);
}

void edt::autosave_component::write(document& doc, tracked& record, document_id id)
{
	const std::uint64_t generation = doc.generation();
	record.due.reset();
	record.written_generation = generation;
	const auto* source = dynamic_cast<const autosave_source*>(&doc);
	if (!source || !is_autosave_kind(doc.kind())) {
		return;
	}
	write_order order;
	order.snapshot = source->autosave_capture();
	if (order.snapshot.bytes.empty() && !order.snapshot.tree) {
		// Asked again only after the next edit: an invalid value or a variant without its base is no failure to repeat.
		const std::string reason = order.snapshot.error.empty() ? std::string{ "nothing to write" } : order.snapshot.error;
		egLOG_DEBUG("editor/autosave", "'{}' ({}) is not autosaved: {}", doc.title(), doc.source().view(), reason);
		report(id, std::string(doc.source().view()), std::format("'{}' is not autosaved: {}", doc.title(), reason));
		return;
	}
	order.root = m_root;
	order.meta.session = m_session;
	order.meta.host = m_host;
	order.file = record.file;
	order.meta.source = autosave_source_text(doc.source());
	order.meta.kind = std::string(doc.kind());
	order.meta.generation = generation;
	if (doc.source().is_valid()) {
		if (const std::optional<res::disk_file> disk = m_ports.resources.disk_file_of(doc.source())) {
			order.source_file = disk->file;
		}
	}
	record.root = m_root;
	record.disk_file = record.file;
	record.on_disk = true;
	record.in_flight = true;
	if (!m_ports.jobs) {
		finish_write(id, run_write(order));
		return;
	}
	auto gate = std::make_shared<write_gate>();
	record.gate = gate;
	m_ports.jobs->start(ds::lane::io, ds::task_desc{ std::format("Autosave {}", doc.title()), "editor/autosave", false }, m_lifetime,
		[order = std::move(order), gate](ds::task_context&) mutable {
			std::lock_guard lock(gate->mutex);
			return gate->cancelled ? std::string{ "cancelled" } : run_write(order);
		},
		[this, id, gate](ds::task_result<std::string>& result) {
			if (gate->cancelled) return;
			finish_write(id, result.ok() && result.value ? *result.value : (result.error.empty() ? std::string{ "the write failed" } : result.error));
		});
}

void edt::autosave_component::finish_write(document_id id, const std::string& error)
{
	const auto found = m_tracked.find(id);
	if (found == m_tracked.end()) return;
	tracked& record = found->second;
	record.in_flight = false;
	record.gate.reset();
	if (error.empty()) {
		++m_writes;
		egLOG_DEBUG("editor/autosave", "autosaved to cache://{}", record.disk_file);
		report(id, {}, {});
	} else {
		egLOG_WARN("editor/autosave", "cache://{} could not be written: {}", record.disk_file, error);
		report(id, record.disk_file, std::format("cache://{} could not be autosaved: {}", record.disk_file, error));
	}
	// Written under a cache folder that is no longer the project's: nothing will ever look for it there.
	if (record.remove_after || m_shut || record.root != m_root) {
		record.remove_after = false;
		discard(record);
	}
	if (record.closed) {
		m_tracked.erase(found);
	}
}

void edt::autosave_component::discard(tracked& record)
{
	if (!record.on_disk) return;
	if (record.in_flight) {
		record.remove_after = true;
		return;
	}
	remove_files(record.root, record.disk_file);
	record.on_disk = false;
}

void edt::autosave_component::forget(document_id id)
{
	report(id, {}, {});
	const auto found = m_tracked.find(id);
	if (found == m_tracked.end()) return;
	discard(found->second);
	if (found->second.in_flight) {
		found->second.closed = true;
		return;
	}
	m_tracked.erase(found);
}

void edt::autosave_component::remove_files(const fs::path& root, const std::string& file) noexcept
{
	if (root.empty() || file.empty()) return;
	try {
		if (const std::string error = remove_autosave_files(root, file); !error.empty()) {
			egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", file, error);
		}
	} catch (...) {
		// Called from shutdown() in a destructor: a failed log line must not end the program.
	}
}

void edt::autosave_component::shutdown()
{
	if (m_shut) return;
	m_shut = true;
	m_closed.disconnect();
	m_reported.clear();
	if (m_ports.problems) m_ports.problems->clear(k_problem_source);
	m_lifetime.cancel_tasks();
	for (auto& [id, record] : m_tracked) {
		if (record.gate) {
			std::lock_guard lock(record.gate->mutex);
			record.gate->cancelled = true;
		}
		record.gate.reset();
		record.in_flight = false;
		discard(record);
	}
	m_tracked.clear();
	end_session();
}

bool edt::autosave_component::ensure_session()
{
	if (m_shut || m_root.empty()) return false;
	if (m_session_root == m_root) return true;
	if (m_session_failed) return false;
	if (start_session()) return true;
	m_session_failed = true;
	return false;
}

bool edt::autosave_component::start_session()
{
	namespace pb = platform::beneath;
	try {
		pb::pinned_root root{ m_root };
		// Folders of format 1 ("autosave/<kind>/") were never released: what is in them is ours and goes.
		const pb::list_result listed = pb::list_beneath(root, fs::path(std::string(k_autosave_folder)), 4096);
		for (const pb::listed_entry& entry : listed.entries) {
			const std::string name = fs_paths::generic_utf8_of(entry.name);
			if (entry.kind != pb::entry_kind::folder || !is_autosave_kind(name)) continue;
			if (const std::string error = remove_autosave_tree(m_root, name); !error.empty()) {
				egLOG_WARN("editor/autosave", "cache://{}/{} (an autosave of an older format) could not be removed: {}", k_autosave_folder, name, error);
			}
		}
		std::string error = "no free session folder was found";
		for (int tries = 0; tries < 8; ++tries) {
			const std::string folder = autosave_session_folder(m_session);
			error = pb::make_folders_beneath(root, fs_paths::path_from_utf8(folder), true);
			if (!error.empty()) {
				std::error_code ec;
				if (!fs::exists(m_root / fs_paths::path_from_utf8(folder), ec)) break;
				m_session = autosave_new_session_id();
				continue;
			}
			const fs::path lock_file = fs_paths::path_from_utf8(folder + "/" + std::string(k_autosave_lock_file));
			platform::lock_attempt attempt = m_lock_port ? m_lock_port(m_root, lock_file, platform::lock_open::create)
				: platform::file_lock::acquire(m_root, lock_file, platform::lock_open::create);
			if (attempt.outcome == platform::lock_outcome::held || attempt.outcome == platform::lock_outcome::missing) {
				// Someone else took the new folder between its making and the lock: it is theirs, nothing of it is touched.
				egLOG_WARN("editor/autosave", "cache://{} was taken by another editor; another session folder is used", folder);
				error = "every new session folder was taken by another editor";
				m_session = autosave_new_session_id();
				continue;
			}
			if (attempt.outcome == platform::lock_outcome::acquired && attempt.lock) {
				m_lock = std::move(attempt.lock);
			} else {
				egLOG_WARN("editor/autosave", "cache://{}/{} could not be locked ({}): another editor cannot tell whether this one still runs",
					folder, k_autosave_lock_file, attempt.error.empty() ? std::string{ "no reason given" } : attempt.error);
			}
			error.clear();
			break;
		}
		if (!error.empty()) {
			egLOG_WARN("editor/autosave", "the autosave folder cache://{} could not be made: {}; autosave is off for this project",
				autosave_session_folder(m_session), error);
			return false;
		}
		autosave_session_info info;
		info.session = m_session;
		info.host = m_host;
		info.pid = platform::current_process_id();
		info.started_at = autosave_time_now();
		info.locked = m_lock.has_value();
		const std::string session_file = autosave_session_folder(m_session) + "/" + std::string(k_autosave_session_file);
		if (const std::string written = write_autosave_file(m_root, session_file, autosave_session_text(info)); !written.empty()) {
			egLOG_WARN("editor/autosave", "cache://{} could not be written: {}; autosave is off for this project", session_file, written);
			m_session_root = m_root;
			end_session();
			return false;
		}
		m_session_root = m_root;
		return true;
	} catch (const std::exception& error) {
		egLOG_WARN("editor/autosave", "the autosave session could not be started: {}", error.what());
		m_lock.reset();
		return false;
	}
}

void edt::autosave_component::end_session() noexcept
{
	if (m_session_root.empty()) return;
	const fs::path root = std::exchange(m_session_root, fs::path{});
	try {
		const std::string folder = autosave_session_folder(m_session);
		static_cast<void>(remove_autosave_file(root, folder + "/" + std::string(k_autosave_session_file)));
		// Removed while the lock is held, so no other editor takes the folder for a dead run's meanwhile.
		std::string error = remove_autosave_tree(root, m_session);
		if (!error.empty() && m_lock) {
			m_lock.reset();
			error = remove_autosave_tree(root, m_session);
		}
		m_lock.reset();
		if (!error.empty()) egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", folder, error);
	} catch (...) {
		// Called from shutdown() in a destructor: a failed log line must not end the program.
		m_lock.reset();
	}
}

void edt::autosave_component::adopt(document_id id, std::uint64_t generation)
{
	document* doc = nullptr;
	m_ports.documents.for_each([&doc, id](document& open) {
		if (open.id() == id) doc = &open;
	});
	if (!doc || m_root.empty() || m_session_root != m_root) return;
	tracked& record = m_tracked[id];
	std::string file = autosave_file_of(m_session, doc->kind(), doc->source());
	if (record.on_disk && !record.in_flight && record.disk_file != file) discard(record);
	record.file = file;
	if (!record.in_flight) {
		record.root = m_root;
		record.disk_file = std::move(file);
		record.on_disk = true;
	}
	record.due.reset();
	record.written_generation = generation;
}

void edt::autosave_component::report(document_id id, std::string file, std::string message)
{
	if (message.empty()) {
		if (m_reported.erase(id) == 0) return;
	} else {
		auto& row = m_reported[id];
		if (row.first == file && row.second == message) return;
		row = { std::move(file), std::move(message) };
	}
	if (!m_ports.problems) return;
	std::vector<problem> items;
	items.reserve(m_reported.size());
	for (const auto& [reported_id, row] : m_reported) {
		problem item;
		item.severity = problem_severity::warning;
		item.file = row.first;
		item.message = row.second;
		items.push_back(std::move(item));
	}
	m_ports.problems->set(k_problem_source, std::move(items));
}
