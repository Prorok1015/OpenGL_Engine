#include "edt_autosave_component.h"
#include "edt_autosave_files.h"
#include "edt_autosave_source.h"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_job_service.h>
#include <snk/model_codec/res_tag.h>
#include <snk/resource/res_system.h>
#include <algorithm>
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

	std::string run_write(write_order& order)
	{
		const std::vector<std::byte> bytes = edt::autosave_bytes(order.snapshot);
		if (bytes.empty()) {
			return "nothing to write";
		}
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
}

edt::autosave_component::autosave_component(ports from)
	: m_ports(std::move(from))
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
	return found != m_tracked.end() && found->second.on_disk ? found->second.file : std::string{};
}

void edt::autosave_component::refresh_root()
{
	const std::vector<fs::path> roots = m_ports.resources.mount_roots(res::tag::cache);
	const fs::path root = roots.empty() ? fs::path{} : roots.back();
	if (root == m_root) {
		return;
	}
	// Another project: what was written for the old one is no autosave of anything open now.
	for (auto& [id, record] : m_tracked) {
		discard(record);
		record.due.reset();
		record.written_generation.reset();
	}
	m_root = root;
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
	const bool can_write = !m_root.empty() && m_settings.enabled && m_allowed;
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
	std::string file = autosave_file_of(doc.kind(), doc.source());
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
		return;
	}
	if (!record.due && record.written_generation != generation) {
		record.due = now + std::chrono::seconds(std::clamp(m_settings.interval_seconds, k_autosave_min_interval, k_autosave_max_interval));
	}
	if (!record.due || now < *record.due || !can_write || record.in_flight || doc.edits_locked()) {
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
	if (!source) {
		return;
	}
	write_order order;
	order.snapshot = source->autosave_capture();
	if (order.snapshot.bytes.empty() && !order.snapshot.tree) {
		egLOG_WARN("editor/autosave", "'{}' was not autosaved: {}", doc.title(), order.snapshot.error.empty() ? "nothing to write" : order.snapshot.error);
		return;
	}
	order.root = m_root;
	order.file = record.file;
	order.meta.source = autosave_source_text(doc.source());
	order.meta.kind = std::string(doc.kind());
	order.meta.generation = generation;
	if (doc.source().is_valid()) {
		if (const std::optional<res::disk_file> disk = m_ports.resources.disk_file_of(doc.source())) {
			order.source_file = disk->file;
		}
	}
	record.on_disk = true;
	record.in_flight = true;
	if (!m_ports.jobs) {
		finish_write(id, run_write(order));
		return;
	}
	m_ports.jobs->start(ds::lane::io, ds::task_desc{ std::format("Autosave {}", doc.title()), "editor/autosave", false }, m_lifetime,
		[order = std::move(order)](ds::task_context&) mutable { return run_write(order); },
		[this, id](ds::task_result<std::string>& result) {
			finish_write(id, result.ok() && result.value ? *result.value : (result.error.empty() ? std::string{ "the write failed" } : result.error));
		});
}

void edt::autosave_component::finish_write(document_id id, const std::string& error)
{
	const auto found = m_tracked.find(id);
	if (found == m_tracked.end()) return;
	tracked& record = found->second;
	record.in_flight = false;
	if (error.empty()) {
		++m_writes;
		egLOG_DEBUG("editor/autosave", "autosaved to cache://{}", record.file);
	} else {
		egLOG_WARN("editor/autosave", "cache://{} could not be written: {}", record.file, error);
	}
	if (record.remove_after || m_shut) {
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
	remove_files(record.file);
	record.on_disk = false;
}

void edt::autosave_component::forget(document_id id)
{
	const auto found = m_tracked.find(id);
	if (found == m_tracked.end()) return;
	discard(found->second);
	if (found->second.in_flight) {
		found->second.closed = true;
		return;
	}
	m_tracked.erase(found);
}

void edt::autosave_component::remove_files(const std::string& file)
{
	if (m_root.empty() || file.empty()) return;
	if (const std::string error = remove_autosave_files(m_root, file); !error.empty()) {
		egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", file, error);
	}
}

void edt::autosave_component::shutdown()
{
	if (m_shut) return;
	m_shut = true;
	m_closed.disconnect();
	m_lifetime.cancel_tasks();
	for (auto& [id, record] : m_tracked) {
		record.in_flight = false;
		discard(record);
	}
	m_tracked.clear();
}
