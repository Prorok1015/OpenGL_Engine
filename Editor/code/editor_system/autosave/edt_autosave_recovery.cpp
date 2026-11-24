#include "edt_autosave_recovery.h"
#include "edt_file_identity.h"
#include <snk/common/fs_paths.h>
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_job_service.h>
#include <snk/platform/plt_beneath.h>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <exception>
#include <format>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace pb = platform::beneath;

namespace
{
	constexpr std::string_view k_prefab_suffix = ".prefab.desc";
	constexpr std::string_view k_poly_suffix = ".poly.desc";
	constexpr std::string_view k_geometry_suffix = ".geom.desc";
	constexpr std::string_view k_desc_suffix = ".desc";

	fs::path relative_of(std::string_view text)
	{
		return fs_paths::path_from_utf8(text);
	}

	std::string name_of(const fs::path& name)
	{
		return fs_paths::generic_utf8_of(name);
	}

	// Why a record's source cannot be the source of its kind; empty when it can (a data record's __type is checked later).
	std::string kind_mismatch(std::string_view kind, std::string_view source)
	{
		const bool desc = source.ends_with(k_desc_suffix);
		const bool special = source.ends_with(k_prefab_suffix) || source.ends_with(k_poly_suffix) || source.ends_with(k_geometry_suffix);
		if (kind == edt::k_autosave_kind_scene) {
			return source.empty() || (desc && !special) ? std::string{} : std::string{ "its source is not a level file" };
		}
		if (kind == edt::k_autosave_kind_prefab) {
			return source.ends_with(k_prefab_suffix) ? std::string{} : std::string{ "its source is not a prefab file" };
		}
		if (kind == edt::k_autosave_kind_mesh) {
			return source.ends_with(k_poly_suffix) ? std::string{} : std::string{ "its source is not a mesh source file" };
		}
		return desc && !special ? std::string{} : std::string{ "its source is not a data document file" };
	}

	struct session_file
	{
		std::optional<edt::autosave_session_info> info;
		bool missing = false;
	};

	session_file read_session_info(const fs::path& root, const std::string& session)
	{
		session_file out;
		std::vector<std::byte> bytes;
		pb::read_request request;
		request.root = root;
		request.folder = relative_of(edt::autosave_session_folder(session));
		request.leaf = relative_of(edt::k_autosave_session_file);
		request.max_bytes = edt::k_recovery_max_meta;
		const pb::read_status status = pb::read_beneath(request, bytes).status;
		out.missing = status == pb::read_status::missing;
		if (status != pb::read_status::ok) return out;
		out.info = edt::parse_autosave_session(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		return out;
	}

	std::string hex_of(const edt::content_digest& digest)
	{
		std::string out;
		out.reserve(digest.size() * 2);
		for (const std::uint8_t byte : digest) out += std::format("{:02x}", static_cast<unsigned>(byte));
		return out;
	}

	struct source_state
	{
		bool exists = false;
		bool changed = false;
		// The hash was not compared: the scan's byte budget was used up.
		bool not_checked = false;
	};

	// Whether the source is there and differs from what the meta recorded: by size first (no read), then by a hash read
	// in chunks whose bytes count against `used`.
	source_state source_now(const std::vector<fs::path>& res_roots, const res::tag& source, const edt::autosave_meta& meta,
		std::uint64_t& used)
	{
		source_state out;
		const fs::path relative = relative_of(source.relative());
		for (auto root = res_roots.rbegin(); root != res_roots.rend(); ++root) {
			std::vector<std::byte> none;
			pb::read_request request;
			request.root = *root;
			request.folder = relative.parent_path();
			request.leaf = relative.filename();
			request.max_bytes = edt::k_recovery_max_payload;
			request.stamp_only = true;
			const pb::read_result stamped = pb::read_beneath(request, none);
			if (stamped.status == pb::read_status::missing) continue;
			out.exists = true;
			if (stamped.status != pb::read_status::ok || !stamped.stamp || meta.source_sha256.empty()) return out;
			const std::uint64_t size = stamped.stamp->size;
			if (meta.source_size && *meta.source_size != size) {
				out.changed = true;
				return out;
			}
			if (used + size > edt::k_recovery_scan_budget) {
				out.not_checked = true;
				return out;
			}
			used += size;
			request.stamp_only = false;
			edt::content_hasher hasher;
			const pb::read_result read = pb::read_beneath_stream(request, [&hasher](std::span<const std::byte> chunk) {
				hasher.update(chunk);
				return true;
			});
			if (read.status == pb::read_status::ok) out.changed = hex_of(hasher.finish()) != meta.source_sha256;
			return out;
		}
		return out;
	}

	class scanner
	{
	public:
		explicit scanner(const edt::recovery_scan_options& options) : m_options(options) {}

		edt::recovery_scan_result run()
		{
			edt::recovery_scan_result out;
			pb::pinned_root root{ m_options.cache_root };
			const pb::list_result listed = pb::list_beneath(root, relative_of(edt::k_autosave_folder), 4096);
			if (listed.missing) return out;
			if (!listed.error.empty()) {
				out.error = listed.error;
				return out;
			}
			out.unchecked_more = listed.truncated;
			std::vector<std::string> names;
			for (const pb::folder_entry& entry : listed.entries) {
				const std::string name = name_of(entry.name);
				if (entry.kind != pb::entry_kind::folder || !edt::is_autosave_session_name(name) || name == m_options.own_session) continue;
				names.push_back(name);
			}
			std::ranges::sort(names);
			if (names.size() > edt::k_recovery_max_sessions) {
				out.unchecked_sessions = names.size() - edt::k_recovery_max_sessions;
				names.resize(edt::k_recovery_max_sessions);
			}
			for (const std::string& name : names) {
				edt::recovery_session session = judge(name);
				if (session.state == edt::recovery_state::live) {
					++out.live;
					continue;
				}
				if (session.state == edt::recovery_state::starting) {
					++out.starting;
					continue;
				}
				read_records(root, session);
				out.sessions.push_back(std::move(session));
			}
			return out;
		}

	private:
		platform::lock_attempt lock(const std::string& session, platform::lock_open open) const
		{
			const fs::path relative = relative_of(edt::autosave_session_folder(session) + "/" + std::string(edt::k_autosave_lock_file));
			if (m_options.lock) return m_options.lock(m_options.cache_root, relative, open);
			return platform::file_lock::acquire(m_options.cache_root, relative, open);
		}

		bool young(const std::string& session) const
		{
			std::error_code ec;
			const fs::file_time_type written = fs::last_write_time(m_options.cache_root / relative_of(edt::autosave_session_folder(session)), ec);
			if (ec) return false;
			const fs::file_time_type now = m_options.now ? m_options.now() : fs::file_time_type::clock::now();
			return now - written < edt::k_recovery_starting_age;
		}

		edt::recovery_session judge(const std::string& id) const
		{
			edt::recovery_session session;
			session.id = id;
			const session_file file = read_session_info(m_options.cache_root, id);
			const std::optional<edt::autosave_session_info>& info = file.info;
			const bool usable_info = info && info->session == id;
			if (usable_info) {
				session.host = info->host;
				session.other_host = info->host != m_options.host;
			}
			// A session that says it holds no lock, or runs on another machine, is never judged by its lock file alone.
			const bool unjudgeable = usable_info && (!info->locked || session.other_host);
			platform::lock_attempt attempt = lock(id, platform::lock_open::existing_only);
			if (attempt.outcome == platform::lock_outcome::missing && !unjudgeable) {
				if (young(id)) {
					session.state = edt::recovery_state::starting;
					return session;
				}
				attempt = lock(id, platform::lock_open::create);
			}
			switch (attempt.outcome) {
			case platform::lock_outcome::held:
				session.state = edt::recovery_state::live;
				return session;
			case platform::lock_outcome::acquired:
				// A lock file without session.json in a young folder: that editor is between its lock and its session.json.
				if (file.missing && young(id)) {
					session.state = edt::recovery_state::starting;
					return session;
				}
				session.lock = std::move(attempt.lock);
				session.state = unjudgeable ? edt::recovery_state::unverifiable : edt::recovery_state::dead;
				return session;
			default:
				session.state = edt::recovery_state::unverifiable;
				return session;
			}
		}

		void read_records(pb::pinned_root& root, edt::recovery_session& session)
		{
			const std::string folder = edt::autosave_session_folder(session.id);
			const pb::list_result kinds = pb::list_beneath(root, relative_of(folder), 64);
			if (!kinds.error.empty()) {
				session.incomplete = "its folder could not be listed: " + kinds.error;
				++session.over_limit;
				return;
			}
			if (kinds.truncated) {
				session.incomplete = "its folder holds more entries than are read";
				++session.over_limit;
			}
			std::size_t metas = 0;
			for (const pb::folder_entry& kind_entry : kinds.entries) {
				const std::string kind = name_of(kind_entry.name);
				if (kind == edt::k_autosave_session_file || kind == edt::k_autosave_lock_file) continue;
				if (kind_entry.kind != pb::entry_kind::folder || !edt::is_autosave_kind(kind)) {
					session.rejected.push_back({ folder + "/" + kind, "not a folder of a known kind" });
					continue;
				}
				const std::string kind_folder = folder + "/" + kind;
				const pb::list_result entries = pb::list_beneath(root, relative_of(kind_folder), 4096);
				if (!entries.error.empty()) {
					session.incomplete = std::format("its folder '{}' could not be listed: {}", kind, entries.error);
					++session.over_limit;
					continue;
				}
				if (entries.truncated) {
					session.incomplete = std::format("its folder '{}' holds more entries than are read", kind);
					++session.over_limit;
				}
				for (const pb::folder_entry& entry : entries.entries) {
					const std::string name = name_of(entry.name);
					if (!name.ends_with(edt::k_autosave_meta_suffix)) continue;
					if (metas >= edt::k_recovery_max_records) {
						++session.over_limit;
						continue;
					}
					++metas;
					check_record(session, kind, kind_folder, name, entry.kind);
				}
			}
		}

		void check_record(edt::recovery_session& session, const std::string& kind, const std::string& kind_folder, const std::string& meta_name,
			pb::entry_kind entry_kind)
		{
			const std::string meta_file = kind_folder + "/" + meta_name;
			const auto reject = [&session, &meta_file](std::string reason) { session.rejected.push_back({ meta_file, std::move(reason) }); };
			if (entry_kind != pb::entry_kind::file) {
				reject("the meta is not a regular file");
				return;
			}
			std::vector<std::byte> meta_bytes;
			pb::read_request request;
			request.root = m_options.cache_root;
			request.folder = relative_of(kind_folder);
			request.leaf = relative_of(meta_name);
			request.max_bytes = edt::k_recovery_max_meta;
			const pb::read_result read = pb::read_beneath(request, meta_bytes);
			if (read.status == pb::read_status::too_large) {
				reject("the meta is larger than 64 KiB");
				return;
			}
			if (read.status != pb::read_status::ok) {
				reject(read.status == pb::read_status::not_regular ? "the meta is not a regular file" : "the meta could not be read");
				return;
			}
			const std::optional<edt::autosave_meta> meta = edt::parse_autosave_meta(
				std::string_view(reinterpret_cast<const char*>(meta_bytes.data()), meta_bytes.size()));
			if (!meta) {
				reject("not a meta of format 2");
				return;
			}
			if (meta->session != session.id) {
				reject(std::format("its session '{}' is not the folder's", meta->session));
				return;
			}
			if (meta->kind != kind || !edt::is_autosave_kind(meta->kind)) {
				reject(std::format("its kind '{}' is not the folder's", meta->kind));
				return;
			}
			std::optional<res::tag> source;
			if (!meta->source.empty()) {
				source = edt::autosave_source_tag(meta->source);
				if (!source) {
					reject(std::format("its source '{}' is not a path inside res://", meta->source));
					return;
				}
			} else if (kind != edt::k_autosave_kind_scene) {
				reject("it names no source");
				return;
			}
			const std::string payload_leaf = meta_name.substr(0, meta_name.size() - edt::k_autosave_meta_suffix.size());
			const std::string expected = edt::autosave_file_of(session.id, kind, source.value_or(res::tag{}));
			if (edt::autosave_leaf_of(expected) != payload_leaf) {
				reject("its file name is not the one of its source");
				return;
			}
			if (meta->payload_size > edt::k_recovery_max_payload) {
				reject("the autosave is larger than 256 MiB");
				return;
			}
			if (m_used + meta->payload_size > edt::k_recovery_scan_budget) {
				++session.not_checked;
				return;
			}
			m_used += meta->payload_size;
			auto payload = std::make_shared<std::vector<std::byte>>();
			pb::read_request payload_request;
			payload_request.root = m_options.cache_root;
			payload_request.folder = relative_of(kind_folder);
			payload_request.leaf = relative_of(payload_leaf);
			payload_request.max_bytes = meta->payload_size;
			const pb::read_result payload_read = pb::read_beneath(payload_request, *payload);
			if (payload_read.status != pb::read_status::ok) {
				reject(payload_read.status == pb::read_status::too_large ? "the autosave's size is not the meta's"
					: payload_read.status == pb::read_status::missing ? "the autosave is missing"
					: payload_read.status == pb::read_status::not_regular ? "the autosave is not a regular file" : "the autosave could not be read");
				return;
			}
			if (payload->size() != meta->payload_size) {
				reject("the autosave's size is not the meta's");
				return;
			}
			if (cmn::sha256_hex(*payload) != meta->payload_sha256) {
				reject("the autosave's SHA-256 is not the meta's");
				return;
			}
			edt::recovery_record record;
			record.file = kind_folder + "/" + payload_leaf;
			record.meta = *meta;
			record.bytes = std::move(payload);
			record.not_restorable = kind_mismatch(kind, meta->source);
			if (source) {
				const source_state now = source_now(m_options.res_roots, *source, *meta, m_used);
				record.source_exists = now.exists;
				record.source_changed = now.changed;
				record.source_not_checked = now.not_checked;
				if (!now.exists && record.not_restorable.empty()) record.not_restorable = "source missing: can only be discarded";
			}
			session.records.push_back(std::move(record));
		}

		const edt::recovery_scan_options& m_options;
		std::uint64_t m_used = 0;
	};

	// Valid UTF-8 sequence length at `at`, 0 when the bytes there are not one.
	std::size_t utf8_length(std::string_view text, std::size_t at)
	{
		const auto byte = [&text](std::size_t i) { return static_cast<unsigned char>(text[i]); };
		const unsigned char lead = byte(at);
		std::size_t length = 0;
		std::uint32_t point = 0;
		if (lead >= 0xC2 && lead <= 0xDF) {
			length = 2;
			point = lead & 0x1Fu;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			length = 3;
			point = lead & 0x0Fu;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			length = 4;
			point = lead & 0x07u;
		} else {
			return 0;
		}
		if (at + length > text.size()) return 0;
		for (std::size_t i = 1; i < length; ++i) {
			if ((byte(at + i) & 0xC0) != 0x80) return 0;
			point = (point << 6) | (byte(at + i) & 0x3Fu);
		}
		const bool overlong = (length == 3 && point < 0x800) || (length == 4 && point < 0x10000);
		const bool surrogate = point >= 0xD800 && point <= 0xDFFF;
		// C1 controls, bidi controls and invisible separators are not shown as themselves.
		const bool hidden = (point >= 0x80 && point <= 0x9F) || (point >= 0x200B && point <= 0x200F) || (point >= 0x202A && point <= 0x202E)
			|| (point >= 0x2060 && point <= 0x2069) || point == 0xFEFF || point > 0x10FFFF;
		return overlong || surrogate || hidden ? 0 : length;
	}
}

edt::recovery_scan_result edt::scan_autosave_sessions(const recovery_scan_options& options)
{
	try {
		return scanner(options).run();
	} catch (const std::exception& error) {
		recovery_scan_result out;
		out.error = error.what();
		return out;
	} catch (...) {
		recovery_scan_result out;
		out.error = "an unknown error";
		return out;
	}
}

std::string edt::recovery_shown_text(std::string_view text)
{
	std::string out;
	std::size_t shown = 0;
	std::size_t at = 0;
	while (at < text.size()) {
		if (shown == k_recovery_max_text) {
			out += "...";
			break;
		}
		const auto byte = static_cast<unsigned char>(text[at]);
		if (byte >= 0x20 && byte < 0x7f) {
			out.push_back(text[at]);
			++at;
		} else if (const std::size_t length = byte >= 0x80 ? utf8_length(text, at) : 0; length > 0) {
			out.append(text.substr(at, length));
			at += length;
		} else {
			out += std::format("\\x{:02X}", static_cast<unsigned>(byte));
			++at;
		}
		++shown;
	}
	return out;
}

std::string_view edt::recovery_kind_label(std::string_view kind)
{
	if (kind == k_autosave_kind_scene) return "Level";
	if (kind == k_autosave_kind_prefab) return "Prefab";
	if (kind == k_autosave_kind_mesh) return "Mesh";
	return "Data";
}

std::string edt::recovery_question_text(const std::vector<const recovery_session*>& sessions, bool unverifiable,
	std::size_t unchecked_sessions, bool unchecked_more)
{
	std::string out = unverifiable
		? "Autosaves of another editor were found, and nothing tells whether that editor still runs:\n"
		: "An editor that did not close normally left unsaved changes:\n";
	std::size_t lines = 0;
	std::size_t more = 0;
	std::size_t rejected = 0;
	std::size_t over_limit = 0;
	std::size_t not_checked = 0;
	const auto line = [&out, &lines, &more](const std::string& text) {
		if (lines < k_recovery_max_lines) {
			out += text;
			out += '\n';
			++lines;
		} else {
			++more;
		}
	};
	for (const recovery_session* session : sessions) {
		if (unverifiable) {
			line(session->other_host && !session->host.empty()
				? std::format("An editor may still be running on {}:", recovery_shown_text(session->host))
				: std::string{ "This file system cannot tell whether that editor still runs:" });
		}
		for (const recovery_record& record : session->records) {
			std::string text = std::format("  {} {}, autosaved {}", recovery_kind_label(record.meta.kind),
				record.meta.source.empty() ? std::string{ "(untitled level)" } : recovery_shown_text(record.meta.source),
				recovery_shown_text(record.meta.saved_at));
			if (record.source_changed) text += " - file changed on disk since the autosave";
			if (record.source_not_checked) text += " - not checked whether the file changed on disk (limit)";
			if (!record.not_restorable.empty()) text += " - " + record.not_restorable;
			line(text);
		}
		if (!session->incomplete.empty()) {
			line(std::format("  cache://{} was not fully read ({}): its folder is kept", autosave_session_folder(session->id),
				recovery_shown_text(session->incomplete)));
		}
		rejected += session->rejected.size();
		over_limit += session->over_limit;
		not_checked += session->not_checked;
	}
	if (more > 0) out += std::format("and {} more\n", more);
	if (rejected > 0) out += std::format("{} autosave(s) failed their checks and cannot be restored (Discard all removes them).\n", rejected);
	if (over_limit > 0) out += std::format("{} autosave(s) were not checked (more than {} in one session).\n", over_limit, k_recovery_max_records);
	if (not_checked > 0) out += std::format("{} autosave(s) were not checked (limit of {} MiB read).\n", not_checked, k_recovery_scan_budget >> 20);
	if (unchecked_sessions > 0 || unchecked_more) {
		out += std::format("{}{} more autosave session(s) were not checked (limit {}).\n", unchecked_sessions, unchecked_more ? " or" : "",
			k_recovery_max_sessions);
	}
	out += "\nRestore all opens them with these changes, unsaved: no file is changed until you save. Discard all deletes these "
		"autosaves. Later keeps them for the next start.";
	return out;
}

edt::autosave_recovery::autosave_recovery(ports from)
	: m_ports(std::move(from))
{
	if (m_ports.events) {
		m_finished = m_ports.events->job_finished.subscribe(
			[this](std::uint64_t id, job_status status, const std::string&, const std::optional<boost::json::value>&) { job_ended(id, status); });
	}
}

edt::autosave_recovery::~autosave_recovery()
{
	m_finished.disconnect();
	m_lifetime.cancel_tasks();
}

void edt::autosave_recovery::job_ended(std::uint64_t id, job_status status)
{
	// job_service calls no on_done of a cancelled job: what waited for it goes on here.
	if (status != job_status::cancelled || id == 0) return;
	if (id == m_scan_job) {
		m_scan_job = 0;
		egLOG_WARN("editor/autosave", "the look for autosaves of other editor runs was cancelled");
		finish();
		return;
	}
	if (id == m_move_job && m_move) {
		m_move_job = 0;
		const pending_move move = *m_move;
		m_move.reset();
		moved(move, "the move was cancelled");
	}
}

void edt::autosave_recovery::install()
{
	if (!m_ports.register_frame) {
		start();
		return;
	}
	m_ports.register_frame("edt/autosave_recovery", [this, alive = std::weak_ptr<int>(m_alive)] {
		if (!alive.lock()) return false;
		if (m_ports.settled && !m_ports.settled()) return true;
		start();
		return false;
	});
}

void edt::autosave_recovery::start()
{
	if (m_started) return;
	m_started = true;
	m_root = m_ports.cache_root ? m_ports.cache_root() : fs::path{};
	if (m_root.empty()) return;
	recovery_scan_options options;
	options.cache_root = m_root;
	options.own_session = m_ports.own_session ? m_ports.own_session() : autosave_session_id();
	options.host = m_ports.host_name ? m_ports.host_name() : this_host_name();
	if (m_ports.res_roots) options.res_roots = m_ports.res_roots();
	options.lock = m_ports.lock;
	options.now = m_ports.now;
	m_busy = true;
	if (!m_ports.jobs) {
		take(std::make_shared<recovery_scan_result>(scan_autosave_sessions(options)));
		return;
	}
	m_scan_job = m_ports.jobs->start(ds::lane::io, ds::task_desc{ "Look for autosaves to recover", "editor/autosave", false }, m_lifetime,
		[options](ds::task_context&) { return std::make_shared<recovery_scan_result>(scan_autosave_sessions(options)); },
		[this](ds::task_result<std::shared_ptr<recovery_scan_result>>& result) {
			m_scan_job = 0;
			if (result.ok() && result.value && *result.value) {
				take(std::move(*result.value));
				return;
			}
			egLOG_WARN("editor/autosave", "the autosaves of other editor runs could not be looked for: {}", recovery_shown_text(result.error));
			finish();
		});
}

void edt::autosave_recovery::take(std::shared_ptr<recovery_scan_result> scan)
{
	m_scan = std::move(scan);
	if (!m_scan->error.empty()) {
		egLOG_WARN("editor/autosave", "the autosaves of other editor runs could not be listed: {}", recovery_shown_text(m_scan->error));
	}
	std::vector<std::size_t> dead;
	std::vector<std::size_t> unverifiable;
	for (std::size_t index = 0; index < m_scan->sessions.size(); ++index) {
		recovery_session& session = m_scan->sessions[index];
		for (const recovery_rejected& rejected : session.rejected) {
			egLOG_WARN("editor/autosave", "cache://{} is not offered for recovery: {}", recovery_shown_text(rejected.file),
				recovery_shown_text(rejected.reason));
		}
		prepare(session);
		if (session.empty()) {
			// A run that died before it wrote anything: nothing to ask about.
			if (session.state == recovery_state::dead && !m_ports.headless) {
				if (const std::string error = remove_autosave_tree(m_root, session.id); !error.empty()) {
					egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", autosave_session_folder(session.id), error);
				}
			}
			session.lock.reset();
			continue;
		}
		(session.state == recovery_state::dead ? dead : unverifiable).push_back(index);
	}
	// One level can be open: only the newest restorable level autosave is restored.
	recovery_record* newest = nullptr;
	for (recovery_session& session : m_scan->sessions) {
		for (recovery_record& record : session.records) {
			if (record.meta.kind != k_autosave_kind_scene || !record.not_restorable.empty()) continue;
			if (newest && newest->meta.saved_at >= record.meta.saved_at) {
				record.not_restorable = "only one level can be open: not restored, kept";
				continue;
			}
			if (newest) newest->not_restorable = "only one level can be open: not restored, kept";
			newest = &record;
		}
	}
	if (m_ports.headless) {
		for (std::size_t index : dead) {
			const recovery_session& session = m_scan->sessions[index];
			egLOG_WARN("editor/autosave", "headless: an editor run that did not close normally left {} autosave(s) in cache://{}; nothing is restored or removed",
				session.records.size(), autosave_session_folder(session.id));
		}
		for (std::size_t index : unverifiable) {
			const recovery_session& session = m_scan->sessions[index];
			egLOG_WARN("editor/autosave", "headless: {} autosave(s) of another editor run in cache://{} (it may still run); nothing is restored or removed",
				session.records.size(), autosave_session_folder(session.id));
		}
		for (const auto& group : { dead, unverifiable }) {
			for (std::size_t index : group) {
				for (const recovery_record& record : m_scan->sessions[index].records) {
					egLOG_WARN("editor/autosave", "headless:   {} {} autosaved {}", recovery_kind_label(record.meta.kind),
						record.meta.source.empty() ? std::string{ "(untitled level)" } : recovery_shown_text(record.meta.source),
						recovery_shown_text(record.meta.saved_at));
				}
			}
		}
		finish();
		return;
	}
	if (!dead.empty()) m_batches.push_back(batch{ std::move(dead), false });
	if (!unverifiable.empty()) m_batches.push_back(batch{ std::move(unverifiable), true });
	if (m_batches.empty() && (m_scan->unchecked_sessions > 0 || m_scan->unchecked_more)) {
		egLOG_WARN("editor/autosave", "{} autosave session(s) were not checked (limit {})", m_scan->unchecked_sessions, k_recovery_max_sessions);
	}
	ask_next();
}

void edt::autosave_recovery::prepare(recovery_session& session)
{
	for (recovery_record& record : session.records) {
		if (record.meta.kind != k_autosave_kind_data || !record.not_restorable.empty()) continue;
		boost::json::error_code ec;
		boost::json::parse_options options;
		options.max_depth = 64;
		const boost::json::value parsed = boost::json::parse(
			std::string_view(reinterpret_cast<const char*>(record.bytes->data()), record.bytes->size()), ec, {}, options);
		const boost::json::value* type = !ec && parsed.is_object() ? parsed.as_object().if_contains("__type") : nullptr;
		const bool document = type && type->is_string() && m_ports.is_document_type && m_ports.is_document_type(std::string_view(type->as_string()));
		if (!document) record.not_restorable = "it is not a data document";
	}
}

void edt::autosave_recovery::ask_next()
{
	if (m_batches.empty()) {
		finish();
		return;
	}
	m_current = std::move(m_batches.front());
	m_batches.erase(m_batches.begin());
	std::vector<const recovery_session*> shown;
	for (std::size_t index : m_current->sessions) shown.push_back(&m_scan->sessions[index]);
	// The count of unchecked sessions goes with the first question only.
	std::string text = recovery_question_text(shown, m_current->unverifiable, m_scan->unchecked_sessions, m_scan->unchecked_more);
	m_scan->unchecked_sessions = 0;
	m_scan->unchecked_more = false;
	std::vector<std::string> options = m_current->unverifiable
		? std::vector<std::string>{ std::string(k_later), std::string(k_restore), std::string(k_discard) }
		: std::vector<std::string>{ std::string(k_restore), std::string(k_discard), std::string(k_later) };
	if (!m_ports.choose) {
		answer(std::nullopt);
		return;
	}
	m_ports.choose(std::string(m_current->unverifiable ? k_title_unverifiable : k_title), std::move(text), std::move(options),
		[this, alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> picked) {
			if (alive.lock()) answer(picked);
		});
}

void edt::autosave_recovery::answer(std::optional<std::size_t> picked)
{
	if (!m_current) return;
	const std::size_t restore = m_current->unverifiable ? 1 : 0;
	const std::size_t discard = m_current->unverifiable ? 2 : 1;
	if (picked && *picked == discard) {
		discard_batch();
		end_batch();
		return;
	}
	if (!picked || *picked != restore) {
		egLOG_INFO("editor/autosave", "the autosaves of other editor runs are kept for the next start");
		end_batch();
		return;
	}
	m_queue.clear();
	for (std::size_t index : m_current->sessions) {
		const recovery_session& session = m_scan->sessions[index];
		for (std::size_t record = 0; record < session.records.size(); ++record) {
			if (session.records[record].not_restorable.empty()) m_queue.emplace_back(index, record);
		}
	}
	restore_next();
}

void edt::autosave_recovery::remove_listed(recovery_session& session)
{
	for (const recovery_record& record : session.records) {
		if (record.restored) continue;
		if (const std::string error = remove_autosave_files(m_root, record.file); !error.empty()) {
			egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", recovery_shown_text(record.file), error);
		}
	}
	for (const recovery_rejected& rejected : session.rejected) {
		static_cast<void>(remove_autosave_file(m_root, rejected.file));
		if (rejected.file.ends_with(k_autosave_meta_suffix)) {
			static_cast<void>(remove_autosave_file(m_root, rejected.file.substr(0, rejected.file.size() - k_autosave_meta_suffix.size())));
		}
	}
}

void edt::autosave_recovery::discard_batch()
{
	for (std::size_t index : m_current->sessions) {
		recovery_session& session = m_scan->sessions[index];
		const std::string folder = autosave_session_folder(session.id);
		// Not dead, or not read whole: only what was looked at goes; the folder, session.json and session.lock stay.
		if (m_current->unverifiable || !session.fully_read()) {
			remove_listed(session);
			egLOG_INFO("editor/autosave", "the autosaves read from cache://{} were discarded; the folder is kept", folder);
			continue;
		}
		std::string error = remove_autosave_tree(m_root, session.id);
		if (!error.empty() && session.lock) {
			// Some file systems refuse to remove a file that is still open locked: once more after the lock went.
			session.lock.reset();
			error = remove_autosave_tree(m_root, session.id);
		}
		if (!error.empty()) egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", folder, error);
		else egLOG_INFO("editor/autosave", "the autosaves in cache://{} were discarded", folder);
	}
}

edt::autosave_recovery::restore_port edt::autosave_recovery::port_of(std::string_view kind) const
{
	if (kind == k_autosave_kind_scene) return m_ports.restore_scene;
	if (kind == k_autosave_kind_prefab) return m_ports.restore_prefab;
	if (kind == k_autosave_kind_mesh) return m_ports.restore_mesh;
	if (kind == k_autosave_kind_data) return m_ports.restore_data;
	return {};
}

void edt::autosave_recovery::restore_next()
{
	if (m_queue.empty()) {
		end_batch();
		return;
	}
	const std::size_t session = m_queue.front().first;
	const std::size_t record = m_queue.front().second;
	m_queue.erase(m_queue.begin());
	const recovery_record& found = m_scan->sessions[session].records[record];
	restore_request request;
	request.kind = found.meta.kind;
	if (!found.meta.source.empty()) request.source = autosave_source_tag(found.meta.source).value_or(res::tag{});
	request.bytes = found.bytes;
	request.leaf = std::string(autosave_leaf_of(found.file));
	request.serial = ++m_serial;
	const restore_port port = port_of(request.kind);
	if (!port) {
		finish_record(session, record, restore_outcome{ "this kind of document cannot be restored here", {}, 0 });
		return;
	}
	port(request, [this, alive = std::weak_ptr<int>(m_alive), session, record](restore_outcome outcome) {
		if (alive.lock()) finish_record(session, record, outcome);
	});
}

void edt::autosave_recovery::finish_record(std::size_t session, std::size_t record, const restore_outcome& outcome)
{
	recovery_record& found = m_scan->sessions[session].records[record];
	const std::string shown = found.meta.source.empty() ? std::string{ "the untitled level" } : recovery_shown_text(found.meta.source);
	if (!outcome.error.empty()) {
		const std::string why = recovery_shown_text(outcome.error);
		egLOG_WARN("editor/autosave", "the autosave of {} was not restored: {}", shown, why);
		if (m_ports.message) {
			m_ports.message(std::format("The autosave of {} could not be restored and is kept:\n{}", shown, why));
		}
		restore_next();
		return;
	}
	egLOG_INFO("editor/autosave", "the autosave of {} was restored (unsaved)", shown);
	const pending_move move{ session, record, outcome.document, outcome.generation };
	if (!m_ports.own_session || (m_ports.ensure_own_session && !m_ports.ensure_own_session())) {
		moved(move, "this run has no autosave folder");
		return;
	}
	const std::string own_session = m_ports.own_session();
	autosave_meta meta = found.meta;
	meta.session = own_session;
	meta.host = m_ports.host_name ? m_ports.host_name() : this_host_name();
	meta.generation = outcome.generation;
	const res::tag source = meta.source.empty() ? res::tag{} : autosave_source_tag(meta.source).value_or(res::tag{});
	const std::string own_file = autosave_file_of(own_session, meta.kind, source);
	const fs::path root = m_root;
	const std::string dead_file = found.file;
	const std::shared_ptr<const std::vector<std::byte>> bytes = found.bytes;
	const std::string meta_text = autosave_meta_text(meta);
	auto relocate = [root, own_file, dead_file, bytes, meta_text]() -> std::string {
		try {
			if (std::string error = write_autosave_files(root, own_file, *bytes, meta_text); !error.empty()) return error;
			return remove_autosave_files(root, dead_file);
		} catch (const std::exception& error) {
			return error.what();
		} catch (...) {
			return "an unknown error";
		}
	};
	if (!m_ports.jobs) {
		moved(move, relocate());
		return;
	}
	m_move = move;
	m_move_job = m_ports.jobs->start(ds::lane::io, ds::task_desc{ "Keep a restored autosave", "editor/autosave", false }, m_lifetime,
		[relocate](ds::task_context&) { return relocate(); },
		[this](ds::task_result<std::string>& result) {
			if (!m_move) return;
			const pending_move done = *m_move;
			m_move.reset();
			m_move_job = 0;
			moved(done, result.ok() && result.value ? *result.value
				: (result.error.empty() ? std::string{ "the move failed" } : result.error));
		});
}

void edt::autosave_recovery::moved(const pending_move& move, const std::string& error)
{
	recovery_record& found = m_scan->sessions[move.session].records[move.record];
	if (error.empty()) {
		found.restored = true;
		if (m_ports.adopt && move.document != document_id{}) m_ports.adopt(move.document, move.generation);
	} else {
		egLOG_WARN("editor/autosave", "the restored autosave cache://{} stays where it is: {}", recovery_shown_text(found.file),
			recovery_shown_text(error));
	}
	restore_next();
}

void edt::autosave_recovery::end_batch()
{
	if (!m_current) return;
	for (std::size_t index : m_current->sessions) {
		recovery_session& session = m_scan->sessions[index];
		const bool restored_all = std::ranges::all_of(session.records, [](const recovery_record& record) { return record.restored; });
		const bool any_restored = std::ranges::any_of(session.records, [](const recovery_record& record) { return record.restored; });
		if (session.state == recovery_state::dead && any_restored && restored_all && session.rejected.empty() && session.fully_read()) {
			std::string error = remove_autosave_tree(m_root, session.id);
			if (!error.empty() && session.lock) {
				session.lock.reset();
				error = remove_autosave_tree(m_root, session.id);
			}
			if (!error.empty()) egLOG_WARN("editor/autosave", "cache://{} could not be removed: {}", autosave_session_folder(session.id), error);
		}
		session.lock.reset();
	}
	m_current.reset();
	ask_next();
}

void edt::autosave_recovery::finish()
{
	if (m_scan) {
		for (recovery_session& session : m_scan->sessions) session.lock.reset();
	}
	m_scan.reset();
	m_current.reset();
	m_batches.clear();
	m_queue.clear();
	m_move.reset();
	m_move_job = 0;
	m_busy = false;
}
