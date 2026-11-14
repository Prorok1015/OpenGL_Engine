#include "edt_migration_commands.h"
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_job_service.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

bool edt::valid_migration_id(std::string_view id)
{
	constexpr std::size_t max_bytes = 128;
	if (id.empty() || id.size() > max_bytes || id.front() == '.') return false;
	return std::ranges::all_of(id, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+';
	});
}

namespace {
	namespace json = boost::json;
	namespace fs = std::filesystem;
	namespace pmg = prj::migration;

	constexpr std::size_t k_max_shown = 32;
	// What a person was shown lives as long as the question about it; an approved plan that met `busy` lives as long again
	// from that refusal, well past agent_confirmations::busy_wait (5 s), so every retry finds it.
	constexpr auto k_shown_lifetime = cmd::command_registry::confirmation_lifetime;
	// How often the job's thread looks at its stop token while it waits for the main thread.
	constexpr std::chrono::milliseconds k_wait_step{ 50 };
	// A wait for the other side of a run (a hop, the end of the apply) logs a warning this often and goes on waiting.
	constexpr std::chrono::seconds k_wait_warning{ 10 };
	constexpr std::size_t k_max_listed = 5;

	constexpr std::string_view k_no_args_schema = R"json({"type":"object","properties":{},"additionalProperties":false})json";

	constexpr std::string_view k_plan_result_schema = R"json({"type":"object","properties":{
		"code":{"type":"integer","description":"what editor --migrate --dry-run exits with: 0 nothing to do, 1 a migration is needed, 2 refused, 4 only files that are never migrated"},
		"plan":{"type":"object"},
		"text":{"type":"string"},
		"fingerprint":{"type":"string","description":"the plan and the project it was made from; pass it to project.migrate as plan_fingerprint"}},
		"required":["code","plan","text","fingerprint"]})json";

	constexpr std::string_view k_migrate_args_schema = R"json({"type":"object","properties":{
		"plan_fingerprint":{"type":"string","minLength":16,"description":"the fingerprint of the plan that was shown (project.migration_plan): the migration is refused when the project no longer gives that plan"}},
		"additionalProperties":false})json";

	// A plan's fingerprint as the commands carry it: 16 lowercase hex digits.
	std::string fingerprint_text(std::uint64_t value)
	{
		return std::format("{:016x}", value);
	}

	std::optional<std::uint64_t> parse_fingerprint(std::string_view text)
	{
		if (text.size() != 16) return std::nullopt;
		std::uint64_t value = 0;
		for (const char c : text) {
			int digit = -1;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
			if (digit < 0) return std::nullopt;
			value = (value << 4) | static_cast<std::uint64_t>(digit);
		}
		return value;
	}

	constexpr std::string_view k_rollback_args_schema = R"json({"type":"object","properties":{
		"id":{"type":"string","minLength":1,"description":"the migration to roll back: a folder name under .snk/migrations (project.migration_plan lists unfinished ones)"}},
		"required":["id"],"additionalProperties":false})json";

	// A journal's report.json is read at most this big (a project of many files keeps a long list).
	constexpr std::uintmax_t k_max_journal_bytes = std::uintmax_t{ 16 } << 20;

	bool valid_migration_id(std::string_view id)
	{
		return edt::valid_migration_id(id);
	}

	constexpr std::string_view k_job_result_schema = R"json({"type":"object","properties":{
		"job":{"type":"integer","minimum":0}},"required":["job"],"additionalProperties":false})json";

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "a project.* schema is not JSON: {}", ec.message());
		return parsed;
	}

	json::value error_value(std::string_view code, std::string_view message)
	{
		return json::object{ { "error", json::object{ { "code", code }, { "message", message } } } };
	}

	// FNV-1a 64.
	class fnv1a_hash
	{
	public:
		void add(std::string_view bytes)
		{
			for (const char c : bytes) {
				m_hash ^= static_cast<unsigned char>(c);
				m_hash *= 0x100000001b3ull;
			}
		}

		// Its size first, so two neighbours never read as one.
		void add_part(std::string_view bytes)
		{
			add(std::to_string(bytes.size()));
			add(":");
			add(bytes);
		}

		// Never follows a link and reads only a regular file up to `max_bytes`, through prj::read_project_file: a link to
		// /dev/zero or a FIFO in the project (or one swapped in after the check) must not hang the editor. Anything else
		// stands in by what it is, its size and its time; so does a file the migration never rewrites (`by_content` false).
		void add_file(const fs::path& root, const fs::path& relative, std::uintmax_t max_bytes, bool by_content)
		{
			const fs::path file = root / relative;
			std::error_code ec;
			const fs::file_status status = fs::symlink_status(file, ec);
			if (ec || status.type() == fs::file_type::not_found) {
				add_part("<missing>");
				return;
			}
			if (fs::is_symlink(status)) {
				add_part("<link>");
				add_part(fs_paths::generic_utf8_of(fs::read_symlink(file, ec)));
				return;
			}
			const std::uintmax_t size = fs::is_regular_file(status) ? fs::file_size(file, ec) : 0;
			if (!fs::is_regular_file(status) || ec || size > max_bytes || !by_content) {
				std::error_code time_error;
				const auto modified = fs::last_write_time(file, time_error);
				add_part(std::format("<{}> {} {}", static_cast<int>(status.type()), ec ? std::uintmax_t{ 0 } : size,
					time_error ? 0LL : static_cast<long long>(modified.time_since_epoch().count())));
				return;
			}
			const prj::project_file_text text = prj::read_project_file(root, relative, max_bytes);
			if (!text.exists || !text.error.empty()) {
				add_part("<unreadable> " + text.error);
				return;
			}
			add(std::to_string(text.text.size()));
			add(":");
			add(text.text);
		}

		std::uint64_t value() const { return m_hash; }

	private:
		std::uint64_t m_hash = 0xcbf29ce484222325ull;
	};

	json::object plan_json(const pmg::migration_plan& plan);

	// What a migration writes: descs and .resource sidecars (and project.snkproj). Textures and models are only stat'ed.
	bool rewritten_by_migration(std::string_view path)
	{
		std::string extension = fs_paths::utf8_of(fs_paths::path_from_utf8(path).extension());
		std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return extension == ".desc" || extension == ".resource";
	}

	// The plan as the answer gives it (every file's steps and warnings, the project steps, what is skipped, the manifest),
	// every byte of the project it was made from, and the code that would migrate it.
	std::uint64_t fingerprint_of(const fs::path& root, const pmg::migration_plan& plan, std::uint64_t code_identity)
	{
		const std::uintmax_t max_bytes = pmg::scan_options{}.max_file_bytes;
		fnv1a_hash hash;
		hash.add_part(std::to_string(code_identity));
		hash.add_part(json::serialize(plan_json(plan)));
		hash.add_file(root, fs_paths::path_from_utf8(prj::k_manifest_file_name), max_bytes, true);
		// Everything the scan says of a file, so a plan that a scan field changes changes the fingerprint too.
		std::vector<std::pair<std::string, std::string>> files;
		files.reserve(plan.scan.files.size());
		for (const pmg::scanned_file& file : plan.scan.files) {
			std::string facts = std::format("{}|{}|{}|{}|{}|{}", pmg::to_string(file.kind), file.reason, file.has_resource, file.root_guid,
				file.resource_path, file.resource_unreadable);
			for (const std::string& type : file.unknown_types) facts += "|" + type;
			files.emplace_back(file.path, std::move(facts));
		}
		std::ranges::sort(files);
		for (const auto& [path, facts] : files) {
			hash.add_part(path);
			hash.add_part(facts);
			hash.add_file(root, fs_paths::path_from_utf8(path), max_bytes, rewritten_by_migration(path));
		}
		return hash.value();
	}

	json::object plan_json(const pmg::migration_plan& plan)
	{
		json::array files;
		for (const pmg::file_plan& file : plan.files) {
			json::array steps;
			for (const pmg::step_use& step : file.steps) {
				steps.push_back(json::object{ { "type", step.type }, { "from", step.from_version }, { "to", step.from_version + 1 }, { "summary", step.summary } });
			}
			json::array warnings;
			for (const desc::migration_warning& warning : file.warnings) {
				warnings.push_back(json::object{ { "type", warning.type }, { "from", warning.from_version }, { "to", warning.from_version + 1 },
					{ "message", warning.message } });
			}
			json::object entry{ { "path", file.path } };
			entry["steps"] = std::move(steps);
			entry["warnings"] = std::move(warnings);
			files.push_back(std::move(entry));
		}
		json::array project_steps;
		for (const pmg::planned_project_step& step : plan.project_steps) {
			project_steps.push_back(json::object{ { "id", step.id }, { "phase", step.phase }, { "summary", step.summary } });
		}
		json::array skipped;
		for (const pmg::skipped_file& file : pmg::skipped_by(plan)) {
			skipped.push_back(json::object{ { "path", file.path }, { "kind", pmg::to_string(file.kind) }, { "reason", file.reason } });
		}
		json::object out;
		out["files"] = std::move(files);
		out["project_steps"] = std::move(project_steps);
		out["manifest"] = json::object{ { "engine_version", plan.scan.manifest.engine_version }, { "outdated", plan.manifest_outdated() },
			{ "completes", plan.completes() } };
		out["skipped"] = std::move(skipped);
		return out;
	}

	// What the journal of a migration says it changed, as a rollback would put it back.
	struct journal_view
	{
		// Why there is nothing to roll back (no journal, one that does not read); empty: it reads.
		std::string error;
		std::string bytes;
		// files[].path, then created_files: what a rollback restores or deletes.
		std::vector<std::string> restored;
		std::vector<std::string> created;
	};

	journal_view read_journal(const fs::path& root, std::string_view id)
	{
		journal_view out;
		if (!valid_migration_id(id)) {
			out.error = "the id does not name a migration";
			return out;
		}
		const prj::project_file_text text = prj::read_project_file(root, fs_paths::path_from_utf8(std::format(".snk/migrations/{}/report.json", id)),
			k_max_journal_bytes);
		if (!text.exists || !text.error.empty()) {
			out.error = std::format("migration {} has no journal{}", id, text.error.empty() ? "" : ": " + text.error);
			return out;
		}
		out.bytes = text.text;
		boost::system::error_code error;
		const json::value journal = json::parse(text.text, error);
		if (error || !journal.is_object()) {
			out.error = std::format("the journal of migration {} cannot be read", id);
			return out;
		}
		const json::object& object = journal.as_object();
		if (const json::value* files = object.if_contains("files"); files && files->is_array()) {
			for (const json::value& file : files->as_array()) {
				const json::value* path = file.is_object() ? file.as_object().if_contains("path") : nullptr;
				if (path && path->is_string()) out.restored.emplace_back(path->as_string());
			}
		}
		if (const json::value* created = object.if_contains("created_files"); created && created->is_array()) {
			for (const json::value& path : created->as_array()) {
				if (path.is_string()) out.created.emplace_back(path.as_string());
			}
		}
		return out;
	}

	// What a person approves for a rollback: the id and the journal's bytes (an edited journal restores other files).
	std::uint64_t rollback_fingerprint(std::string_view id, const journal_view& journal)
	{
		fnv1a_hash hash;
		hash.add_part("rollback");
		hash.add_part(id);
		hash.add_part(journal.error);
		hash.add_part(journal.bytes);
		return hash.value();
	}

	// At most k_max_listed paths of a journal, each cut short and without control characters: a journal is a file anyone
	// can edit, and a line break in a path must not forge a line of the question a person reads.
	std::string listed(const std::vector<std::string>& paths)
	{
		std::string out;
		for (std::size_t i = 0; i < paths.size() && i < k_max_listed; ++i) {
			std::string path = paths[i].size() > 200 ? paths[i].substr(0, 200) + "..." : paths[i];
			std::ranges::replace_if(path, [](char c) {
				const auto byte = static_cast<unsigned char>(c);
				return byte < 0x20 || byte == 0x7f;
			}, '?');
			out += (i == 0 ? " " : ", ") + path;
		}
		if (paths.size() > k_max_listed) out += ", ...";
		return out;
	}

	std::string unfinished_refusal(const std::vector<std::string>& unfinished)
	{
		return std::format("migration {} did not finish: run editor --migrate-rollback {} first", unfinished.front(), unfinished.front());
	}

	json::array strings_of(const std::vector<std::string>& items)
	{
		json::array out;
		for (const std::string& item : items) out.push_back(json::string(item));
		return out;
	}

	// The res:// tag of a project path (relative to the root, '/') below one of the content roots; none outside them.
	std::optional<res::tag> tag_below_content_root(const fs::path& root, const std::vector<fs::path>& content_roots, std::string_view path)
	{
		const fs::path file = (root / fs_paths::path_from_utf8(path)).lexically_normal();
		for (const fs::path& content_root : content_roots) {
			const fs::path relative = file.lexically_relative(content_root.lexically_normal());
			if (relative.empty() || relative.is_absolute() || relative == "." || *relative.begin() == "..") continue;
			return res::tag::make(fs_paths::generic_utf8_of(relative));
		}
		return std::nullopt;
	}

	// What apply_migration came to on the io lane: its outcome, or the exception that stopped it.
	struct apply_result
	{
		pmg::apply_outcome outcome;
		std::optional<std::string> stopped;
		// A rollback: why it did not finish (rollback_migration's error); nullopt when it did.
		std::optional<std::string> rollback_error;
		// A rollback the job's stop reached before it began: nothing was written.
		bool not_run = false;
		// rollback_context::wrote: the rollback wrote something (its journal first).
		bool rollback_wrote = false;
	};

	// Where the job's io work and the main thread meet (#761): the main thread hands the apply over once, the io work runs
	// it and hands the result back; meanwhile the apply's on_main hops wait here for the next frame of the main thread.
	class apply_handoff
	{
	public:
		// Main thread, frame 2: false when the job was abandoned first. From here on the job waits for the apply.
		bool claim()
		{
			std::lock_guard lock(m_mutex);
			if (m_closed) return false;
			m_handed = true;
			return true;
		}

		void hand(std::function<apply_result()> apply)
		{
			{
				std::lock_guard lock(m_mutex);
				m_apply = std::move(apply);
			}
			m_changed.notify_all();
		}

		// The job: the apply once it was handed over, waiting at most `step` for it.
		std::function<apply_result()> take(std::chrono::milliseconds step)
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait_for(lock, step, [this] { return static_cast<bool>(m_apply); });
			std::function<apply_result()> apply = std::exchange(m_apply, {});
			if (apply) m_running = true;
			return apply;
		}

		void finish(apply_result result)
		{
			{
				std::lock_guard lock(m_mutex);
				m_running = false;
				m_over = true;
				m_result = std::move(result);
			}
			m_changed.notify_all();
		}

		// The job's stop token (job.cancel, job_service::cancel_all_and_wait when the editor quits) or the job's body dropped:
		// every hop, waiting or later, is refused at once, so an apply that runs fails fast and rolls back instead of waiting
		// for a main thread that is itself waiting for this job.
		void refuse_hops()
		{
			std::lock_guard lock(m_mutex);
			m_closed = true;
			m_stop_seen = true;
			for (const auto& item : m_hops) item->done.set_value(false);
			m_hops.clear();
		}

		// The job's work is gone (finished, or dropped unrun by a cancel before it started).
		void job_ended()
		{
			refuse_hops();
			std::lock_guard lock(m_mutex);
			m_job_gone = true;
		}

		// Main thread: the job ended without taking the apply that was handed to it; nothing was written.
		bool ended_unrun()
		{
			std::lock_guard lock(m_mutex);
			return m_job_gone && !m_over && !m_running;
		}

		// apply_context::stop_requested, on the io lane. A false answer at `commit` commits the migration: from then on a
		// cancel is refused (cancel_veto) and the job ends with the answer.
		bool stop_requested(pmg::apply_checkpoint where)
		{
			std::lock_guard lock(m_mutex);
			if (m_stop_seen) return true;
			if (where == pmg::apply_checkpoint::commit) m_committed = true;
			return false;
		}

		// job_service's veto of a cancel (main thread): refused once committed. Only when `decide` (cancel() itself) is the
		// answer acted on: a refused cancel is noted for the answer, one let through is seen by the next checkpoint at once,
		// before the stop token reaches the job, so it cannot slip past the commit.
		std::optional<std::string> cancel_veto(bool decide)
		{
			std::lock_guard lock(m_mutex);
			if (m_committed) {
				if (decide) m_cancel_ignored = true;
				return std::string{ "the migration was committed: it ends and reports what it wrote" };
			}
			if (decide) m_stop_seen = true;
			return std::nullopt;
		}

		bool cancel_ignored()
		{
			std::lock_guard lock(m_mutex);
			return m_cancel_ignored;
		}

		// The job saw its stop token: true when nothing was handed over yet, and then nothing ever will be.
		bool abandon()
		{
			std::lock_guard lock(m_mutex);
			if (m_handed) return false;
			m_closed = true;
			return true;
		}

		// Whether the job has nothing more to run: the apply was never handed over, or it ended.
		bool apply_over()
		{
			std::lock_guard lock(m_mutex);
			return !m_handed || m_over;
		}

		// apply_context::on_main, on the io lane: waits until the main thread ran `fn`; false at once when it never will.
		bool run_on_main(const std::function<void()>& fn)
		{
			auto item = std::make_shared<hop>();
			item->fn = &fn;
			std::future<bool> ran = item->done.get_future();
			{
				std::lock_guard lock(m_mutex);
				if (m_closed) return false;
				m_hops.push_back(item);
			}
			// Answered by the main thread's frame, or refused by refuse_hops/close.
			while (ran.wait_for(k_wait_warning) != std::future_status::ready) {
				egLOG_WARN("project/migrate", "the migration waits for the main thread to load a desc");
			}
			return ran.get();
		}

		// Main thread, every frame of the run: the hops queued since the last one.
		void pump()
		{
			std::deque<std::shared_ptr<hop>> hops;
			{
				std::lock_guard lock(m_mutex);
				hops.swap(m_hops);
			}
			for (const auto& item : hops) {
				try {
					(*item->fn)();
					item->done.set_value(true);
				} catch (...) {
					item->done.set_exception(std::current_exception());
				}
			}
		}

		// Main thread: the result, once.
		std::optional<apply_result> take_result()
		{
			std::lock_guard lock(m_mutex);
			return std::exchange(m_result, std::nullopt);
		}

		// Main thread, the editor going away: every hop, waiting or later, is refused, an apply not taken yet never runs,
		// and one that runs is waited for (it fails fast: it cannot reach the main thread any more).
		void close()
		{
			std::unique_lock lock(m_mutex);
			m_closed = true;
			m_stop_seen = true;
			for (const auto& item : m_hops) item->done.set_value(false);
			m_hops.clear();
			if (m_apply) {
				m_apply = {};
				m_over = true;
			}
			// The apply uses what goes with the editor: it must end first. It cannot reach the main thread any more.
			while (!m_changed.wait_for(lock, k_wait_warning, [this] { return !m_running; })) {
				egLOG_WARN("project/migrate", "the editor waits for the migration to end (it rolls back)");
			}
		}

	private:
		struct hop
		{
			const std::function<void()>* fn = nullptr;
			std::promise<bool> done;
		};

		std::mutex m_mutex;
		std::condition_variable m_changed;
		bool m_handed = false;
		bool m_closed = false;
		bool m_running = false;
		bool m_over = false;
		bool m_job_gone = false;
		bool m_stop_seen = false;
		bool m_committed = false;
		bool m_cancel_ignored = false;
		std::function<apply_result()> m_apply;
		std::optional<apply_result> m_result;
		std::deque<std::shared_ptr<hop>> m_hops;
	};

	// Lives as long as the job's work (a copy of it in the task): its end, run or dropped unrun, is told to the handoff.
	struct job_presence
	{
		explicit job_presence(std::weak_ptr<apply_handoff> to) : handoff(std::move(to)) {}
		job_presence(const job_presence&) = delete;
		job_presence& operator=(const job_presence&) = delete;
		~job_presence()
		{
			if (const auto alive = handoff.lock()) alive->job_ended();
		}

		std::weak_ptr<apply_handoff> handoff;
	};

	// One project.migrate (or project.migrate_rollback) from the job's start to its answer; the steps run in the implicit
	// frame callback, the apply (or the rollback) in the job's io work.
	struct migration_run
	{
		std::promise<json::value> promise;
		// Set by the job when it was cancelled: a run that has not written yet stops without writing.
		std::shared_ptr<std::atomic<bool>> abandoned = std::make_shared<std::atomic<bool>>(false);
		fs::path root;
		// What the person was shown (the ticket's record, or the plan_fingerprint of a person's call); none: nothing to check.
		std::optional<std::uint64_t> shown;
		// The fingerprint came with a person's call (the window): a change since is `plan_changed`, not `denied`.
		bool shown_to_caller = false;
		int frame = 0;
		res::tag level;
		bool modal_closed = false;
		bool load_started = false;
		bool load_ended = false;
		bool loaded = false;
		std::optional<std::string> reopen_error;
		json::object answer;
		bool fulfilled = false;
		std::shared_ptr<apply_handoff> handoff = std::make_shared<apply_handoff>();
		// From the hand-off to the result: the main thread pumps the hops and waits.
		bool applying = false;
		bool writes = false;
		// migration_deps::running_changed(true) was said.
		bool running_said = false;
		std::shared_ptr<const pmg::migration_plan> plan;
		// What the mounts report waits from before the first write until the result is in (then announce_changes).
		res::change_hold hold;
		// project.migrate_rollback of migration `id`; what its journal names is announced after it.
		bool rollback = false;
		std::string id;
		std::vector<std::string> journal_files;

		void fulfil(json::value value)
		{
			if (fulfilled) return;
			fulfilled = true;
			promise.set_value(std::move(value));
		}
	};

	class migration_commands : public std::enable_shared_from_this<migration_commands>
	{
	public:
		explicit migration_commands(edt::migration_deps deps) : m_deps(std::move(deps))
		{
			pmg::register_project_steps(m_steps);
		}

		// The editor goes away during a run: the apply's hops fail at once and the apply, which then rolls back, is waited
		// for before what it uses goes; the job ends with a failure.
		~migration_commands()
		{
			if (!m_running) return;
			m_running->handoff->close();
			m_running->hold.release();
		}

		migration_commands(const migration_commands&) = delete;
		migration_commands& operator=(const migration_commands&) = delete;

		cmd::result plan(const cmd::call&) const
		{
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root) return cmd::result::failure(k_no_project, "no project is open");
			const pmg::migration_plan plan = make_plan(*root);
			json::object planned = plan_json(plan);
			const std::vector<std::string> unfinished = pmg::unfinished_migrations(*root);
			const std::string refused = !unfinished.empty() ? unfinished_refusal(unfinished) : pmg::plan_refusal(plan);
			if (!refused.empty()) planned["refused"] = refused;
			if (!unfinished.empty()) planned["unfinished"] = strings_of(unfinished);
			int code = pmg::k_exit_done;
			if (!refused.empty()) code = pmg::k_exit_refused;
			else if (!plan.empty()) code = pmg::k_exit_migration_needed;
			else if (!pmg::skipped_by(plan).empty()) code = pmg::k_exit_partial;
			return cmd::result::success(json::object{ { "code", code }, { "plan", std::move(planned) }, { "text", pmg::plan_text(plan) },
				{ "fingerprint", fingerprint_text(fingerprint_of(*root, plan, code_identity())) } });
		}

		std::optional<std::string> describe(const cmd::call& call)
		{
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root) return std::string("This request will be refused: no project is open");
			const pmg::migration_plan plan = make_plan(*root);
			remember(key_of(call.from), call.confirmation, fingerprint_of(*root, plan, code_identity()));

			const std::string name = m_deps.project_name ? m_deps.project_name() : std::string{};
			std::string text = std::format("Migrate the project '{}' in place\n  {}\n  {} files, {} project steps", name,
				fs_paths::generic_utf8_of(*root), plan.files.size(), plan.project_steps.size());
			if (plan.manifest_outdated()) {
				text += std::format("; engine {} -> {}", plan.scan.manifest.engine_version, plan.completes() ? "this engine" : "kept (files of an unloaded module stay)");
			}
			text += "\n";
			const std::vector<pmg::skipped_file> skipped = pmg::skipped_by(plan);
			if (!skipped.empty()) {
				text += std::format("  Not migrated ({}):", skipped.size());
				for (std::size_t i = 0; i < skipped.size() && i < k_max_listed; ++i) {
					text += std::format(" {} ({})", skipped[i].path, pmg::to_string(skipped[i].kind));
				}
				if (skipped.size() > k_max_listed) text += " ...";
				text += "\n";
			}
			const std::vector<std::string> unfinished = pmg::unfinished_migrations(*root);
			const std::string refused = !unfinished.empty() ? unfinished_refusal(unfinished) : pmg::plan_refusal(plan);
			if (!refused.empty()) text += "  This request will be refused: " + refused + "\n";
			if (plan.empty() && refused.empty()) text += "  Nothing to migrate.\n";
			text += "Open documents are closed, and the level is closed and opened again from disk. "
				"Every file is backed up under .snk/migrations/ first.";
			return text;
		}

		cmd::result migrate(const cmd::call& call)
		{
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root) return cmd::result::failure(k_no_project, "no project is open");
			// A person's own call needs no plan shown, as Play and the forge calls of a person: the person in the editor is
			// the confirmation.
			const bool person = call.from.kind == cmd::origin_kind::ui;
			const std::string key = key_of(call.from);
			const bool retry = m_deps.approved_internally && m_deps.approved_internally(call.confirmation);
			shown_plan* shown = person ? nullptr : find_shown(key, call.confirmation, retry);
			if (!person && !shown) {
				return cmd::result::failure(cmd::codes::denied, retry ? "the plan the person approved is no longer kept; ask again"
					: "the plan was not shown to a person; ask again");
			}
			// plan_fingerprint only ever adds a check: an agent's run stays bound to the plan its ticket recorded.
			std::optional<std::uint64_t> passed;
			if (const json::value* given = call.args.is_object() ? call.args.as_object().if_contains("plan_fingerprint") : nullptr) {
				passed = given->is_string() ? parse_fingerprint(given->as_string()) : std::nullopt;
				if (!passed) return cmd::result::failure(cmd::codes::invalid_args, "plan_fingerprint is not a fingerprint project.migration_plan gives");
				if (shown && *passed != shown->fingerprint) {
					return cmd::result::failure(cmd::codes::denied, "plan_fingerprint is not the plan the person approved; ask again");
				}
			}
			// Refused for now: a retry (agent_confirmations repeats a `busy` with a fresh ticket, whose description sees the
			// project as it is then) is still judged by the plan the person approved.
			const auto refuse = [this, shown, &key](std::string code, std::string message) {
				if (shown) keep_approved(shown, key);
				return cmd::result::failure(std::move(code), std::move(message));
			};
			if (m_running) return refuse(cmd::codes::busy, "a migration of the project is already running");
			if (auto why = m_deps.busy_reason ? m_deps.busy_reason() : std::nullopt) return refuse(cmd::codes::busy, *why);
			if (auto why = m_deps.unsaved_reason ? m_deps.unsaved_reason() : std::nullopt) return refuse(cmd::codes::needs_input, *why);

			auto run = std::make_shared<migration_run>();
			run->root = *root;
			if (shown) {
				run->shown = shown->fingerprint;
				std::erase_if(m_shown, [&key](const shown_plan& entry) { return entry.key == key; });
			} else if (passed) {
				run->shown = passed;
				run->shown_to_caller = true;
			}
			return start_run(std::move(run), "Migrate project");
		}

		std::optional<std::string> describe_rollback(const cmd::call& call)
		{
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root) return std::string("This request will be refused: no project is open");
			const std::string id = id_of(call);
			const journal_view journal = read_journal(*root, id);
			remember(rollback_key(call.from), call.confirmation, rollback_fingerprint(id, journal));

			const std::string name = m_deps.project_name ? m_deps.project_name() : std::string{};
			std::string text = std::format("Roll back a migration of the project '{}'\n  {}\n", name, fs_paths::generic_utf8_of(*root));
			if (!journal.error.empty()) {
				text += "  This request will be refused: " + journal.error + "\n";
				return text;
			}
			text += std::format("  Migration {}: .snk/migrations/{}/\n", id, id);
			text += std::format("  Puts back from its backup ({}):{}\n", journal.restored.size(), listed(journal.restored));
			if (!journal.created.empty()) text += std::format("  Deletes the files it created ({}):{}\n", journal.created.size(), listed(journal.created));
			text += "A file changed since the migration wrote it refuses the whole rollback; nothing is written then. "
				"Open documents are closed, and the level is closed and opened again from disk.";
			return text;
		}

		cmd::result rollback(const cmd::call& call)
		{
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root) return cmd::result::failure(k_no_project, "no project is open");
			const std::string id = id_of(call);
			if (!valid_migration_id(id)) {
				return cmd::result::failure(cmd::codes::invalid_args, "id is not the name of a migration (a folder under .snk/migrations)");
			}
			// As project.migrate: a person's own call is its own confirmation; an agent's is bound to the journal it was shown.
			const bool person = call.from.kind == cmd::origin_kind::ui;
			const std::string key = rollback_key(call.from);
			const bool retry = m_deps.approved_internally && m_deps.approved_internally(call.confirmation);
			shown_plan* shown = person ? nullptr : find_shown(key, call.confirmation, retry);
			if (!person && !shown) {
				return cmd::result::failure(cmd::codes::denied, retry ? "the rollback the person approved is no longer kept; ask again"
					: "the rollback was not shown to a person; ask again");
			}
			const auto refuse = [this, shown, &key](std::string code, std::string message) {
				if (shown) keep_approved(shown, key);
				return cmd::result::failure(std::move(code), std::move(message));
			};
			if (m_running) return refuse(cmd::codes::busy, "a migration or a rollback of the project is already running");
			if (auto why = m_deps.busy_reason ? m_deps.busy_reason() : std::nullopt) return refuse(cmd::codes::busy, *why);
			if (auto why = m_deps.unsaved_reason ? m_deps.unsaved_reason() : std::nullopt) return refuse(cmd::codes::needs_input, *why);

			auto run = std::make_shared<migration_run>();
			run->root = *root;
			run->rollback = true;
			run->id = id;
			if (shown) {
				run->shown = shown->fingerprint;
				std::erase_if(m_shown, [&key](const shown_plan& entry) { return entry.key == key; });
			}
			return start_run(std::move(run), "Roll back migration");
		}

	private:
		static constexpr const char* k_no_project = "no_project";

		// The job of a run: its io work runs the apply (or the rollback) the main thread hands over, and otherwise waits for
		// the answer; the steps run in the implicit frame callback.
		cmd::result start_run(std::shared_ptr<migration_run> run, std::string label)
		{
			// The io work runs the apply the main thread hands over, and otherwise waits for the answer. Once the apply was
			// handed over it is never abandoned: it runs to its end (a stop refuses its hops, so it rolls back), and the main
			// thread announces what it wrote.
			auto work = [future = run->promise.get_future().share(), abandoned = run->abandoned, handoff = run->handoff,
							presence = std::make_shared<job_presence>(run->handoff)](ds::task_context& task) -> json::value {
				// A stop never waits for the main thread: the editor quitting waits for this job (cancel_all_and_wait) first.
				const std::stop_callback refuse(task.stop, [handoff] { handoff->refuse_hops(); });
				static_cast<void>(presence);
				try {
					while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
						if (std::function<apply_result()> apply = handoff->take(k_wait_step)) {
							handoff->finish(apply());
							continue;
						}
						if (task.stop.stop_requested()) {
							if (handoff->abandon()) {
								abandoned->store(true);
								throw ds::task_cancelled{};
							}
							if (handoff->apply_over()) {
								return error_value(cmd::codes::failed, "the migration ended without an answer (the job was cancelled or the editor "
									"stopped): see .snk/migrations");
							}
						}
					}
					return future.get();
				} catch (const std::future_error&) {
					return error_value(cmd::codes::failed, "the editor stopped before the migration ended");
				}
			};
			const std::uint64_t job = m_deps.jobs.start_with_result(ds::lane::io, ds::task_desc{ label, "project", true }, m_lifetime, std::move(work));
			// Before the commit a cancel rolls the migration back (a rollback: it does not begin); after it the cancel is
			// refused and the answer says so.
			m_deps.jobs.set_cancel_veto(job, [handoff = run->handoff](bool decide) { return handoff->cancel_veto(decide); });
			m_running = run;
			egLOG_INFO("project/migrate", "{} of {} started (job {})", run->rollback ? "rollback " + run->id : std::string("migration"),
				fs_paths::generic_utf8_of(run->root), job);
			m_deps.register_implicit(edt::k_migration_frame_id, [weak = weak_from_this(), run] {
				const auto self = weak.lock();
				return self && self->step(run);
			});
			return cmd::result::success(json::object{ { "job", static_cast<std::int64_t>(job) } });
		}

		static constexpr const char* k_plan_changed = "plan_changed";

		// A plan a person was shown, by caller and by the ticket its question got (empty: described for a call allowed
		// without a question). `approved`: a run the person approved was refused for now (busy): this record alone judges
		// the retries, a later question of the caller does not replace it and nothing evicts it.
		struct shown_plan
		{
			std::string key;
			std::string ticket;
			std::uint64_t fingerprint = 0;
			std::chrono::steady_clock::time_point at;
			bool approved = false;
		};

		std::chrono::steady_clock::time_point now() const
		{
			return m_deps.now ? m_deps.now() : std::chrono::steady_clock::now();
		}

		static std::string key_of(const cmd::origin& from)
		{
			return std::format("{}|{}", static_cast<int>(from.kind), from.session);
		}

		// A rollback's records stand beside the caller's plan, never in place of it.
		static std::string rollback_key(const cmd::origin& from)
		{
			return key_of(from) + "|rollback";
		}

		static std::string id_of(const cmd::call& call)
		{
			const json::value* id = call.args.is_object() ? call.args.as_object().if_contains("id") : nullptr;
			return id && id->is_string() ? std::string(id->as_string()) : std::string{};
		}

		void forget_expired()
		{
			const auto at = now();
			std::erase_if(m_shown, [&](const shown_plan& shown) { return at - shown.at > k_shown_lifetime; });
		}

		std::uint64_t code_identity() const
		{
			return m_deps.code_identity ? m_deps.code_identity() : 0;
		}

		// A new question of the caller replaces its unanswered one, never an approved one. Only unanswered records count
		// against k_max_shown (the oldest goes), so a question is always recorded: approved ones are at most one per caller,
		// each made by a person's answer, and none is evicted.
		void remember(std::string key, std::string ticket, std::uint64_t value)
		{
			forget_expired();
			std::erase_if(m_shown, [&](const shown_plan& shown) { return shown.key == key && !shown.approved; });
			if (std::ranges::count_if(m_shown, [](const shown_plan& shown) { return !shown.approved; }) >= static_cast<std::ptrdiff_t>(k_max_shown)) {
				m_shown.erase(std::ranges::find_if(m_shown, [](const shown_plan& shown) { return !shown.approved; }));
			}
			m_shown.push_back({ std::move(key), std::move(ticket), value, now(), false });
		}

		// The run of `shown` was approved and refused for now: it becomes the caller's one approved record, with a lifetime
		// counted from now.
		void keep_approved(shown_plan* shown, const std::string& key)
		{
			shown->approved = true;
			shown->at = now();
			std::erase_if(m_shown, [&](const shown_plan& entry) { return entry.key == key && entry.approved && &entry != shown; });
		}

		// A retry agent_confirmations approved itself: the caller's approved record only, never one described for the
		// retry's own ticket. A ticket a person answered: the record of that ticket. No ticket (an answer given in
		// advance): the approved record first, else the one described for the call.
		shown_plan* find_shown(const std::string& key, const std::string& ticket, bool retry)
		{
			forget_expired();
			const auto approved = std::ranges::find_if(m_shown, [&](const shown_plan& shown) { return shown.key == key && shown.approved; });
			if (retry) return approved != m_shown.end() ? &*approved : nullptr;
			if (ticket.empty() && approved != m_shown.end()) return &*approved;
			const auto asked = std::ranges::find_if(m_shown, [&](const shown_plan& shown) { return shown.key == key && shown.ticket == ticket; });
			return asked != m_shown.end() ? &*asked : nullptr;
		}

		pmg::migration_plan make_plan(const fs::path& root) const
		{
			const desc::desc_system& descs = m_deps.descs();
			pmg::plan_options options;
			options.project_steps = &m_steps;
			options.force = false;
			return pmg::plan_migration(root, pmg::scan_project(root, descs, options.scan), descs, options);
		}

		void show_progress(bool open) const
		{
			if (m_deps.show_progress) m_deps.show_progress(open);
		}

		void say_running(migration_run& run, bool running)
		{
			if (run.running_said == running) return;
			run.running_said = running;
			if (m_deps.running_changed) m_deps.running_changed(running);
		}

		void end_run(migration_run& run)
		{
			run.hold.release();
			say_running(run, false);
			m_running.reset();
		}

		// Ends a run that loads no level again.
		bool stop(migration_run& run, json::value answer)
		{
			show_progress(false);
			run.fulfil(std::move(answer));
			end_run(run);
			return false;
		}

		// One frame of the run: the modal, then the migration, then the wait for the level. False when it is over.
		bool step(const std::shared_ptr<migration_run>& current)
		{
			migration_run& run = *current;
			++run.frame;
			if (run.frame == 1) {
				show_progress(true);
				return true;
			}
			if (run.frame == 2) return run.rollback ? rollback_now(current) : migrate_now(current);
			if (run.applying) {
				show_progress(true);
				run.handoff->pump();
				std::optional<apply_result> result = run.handoff->take_result();
				if (!result) {
					// The job was cancelled before it ran (its body dropped unrun): nobody takes the apply, so nothing is
					// written; the level comes back as after a refused plan.
					if (!run.handoff->ended_unrun()) return true;
					run.handoff->close();
					egLOG_WARN("project/migrate", "the job was cancelled before the migration ran: nothing was written");
					result = apply_result{};
					result->outcome.refused = "the job was cancelled before the migration ran: nothing was written";
					result->not_run = true;
				}
				run.applying = false;
				return after_apply(current, std::move(*result));
			}
			if (!run.modal_closed) {
				show_progress(false);
				run.modal_closed = true;
			}
			const bool loading = m_deps.level_load_in_progress && m_deps.level_load_in_progress();
			if (run.load_started && !run.load_ended && loading) return true;
			finish(run);
			return false;
		}

		// Frame 2 of a run, before anything is written: why it ends now; nullopt when it goes on.
		std::optional<json::value> refusal_now(const migration_run& run) const
		{
			if (run.abandoned->load()) return error_value(cmd::codes::failed, "the job was cancelled: nothing was written");
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root || *root != run.root) return error_value(k_no_project, "the project is not open any more: nothing was written");
			if (auto why = m_deps.busy_reason ? m_deps.busy_reason() : std::nullopt) return error_value(cmd::codes::busy, *why);
			if (auto why = m_deps.unsaved_reason ? m_deps.unsaved_reason() : std::nullopt) return error_value(cmd::codes::needs_input, *why);
			return std::nullopt;
		}

		// Before the first write: the documents and the level let go, the level's watch paused, the mounts' reports held.
		void begin_writes(migration_run& run)
		{
			run.level = m_deps.close_documents_and_detach_level ? m_deps.close_documents_and_detach_level() : res::tag{};
			if (m_deps.pause_watch) m_deps.pause_watch();
			run.hold = m_deps.resources().hold_changes();
		}

		bool migrate_now(const std::shared_ptr<migration_run>& current)
		{
			migration_run& run = *current;
			show_progress(true);
			if (std::optional<json::value> refused = refusal_now(run)) return stop(run, std::move(*refused));

			const pmg::migration_plan plan = make_plan(run.root);
			if (run.shown && fingerprint_of(run.root, plan, code_identity()) != *run.shown) {
				if (run.shown_to_caller) return stop(run, error_value(k_plan_changed, "the project changed since the plan was shown: nothing was written"));
				return stop(run, error_value(cmd::codes::denied, "the project changed since the question: ask again"));
			}
			const std::vector<std::string> unfinished = pmg::unfinished_migrations(run.root);
			const std::string refused = !unfinished.empty() ? unfinished_refusal(unfinished) : pmg::plan_refusal(plan);
			if (!refused.empty()) {
				json::object answer{ { "code", pmg::k_exit_refused }, { "refused", refused }, { "text", pmg::plan_text(plan) } };
				if (!unfinished.empty()) answer["unfinished"] = strings_of(unfinished);
				return stop(run, std::move(answer));
			}

			if (!run.handoff->claim()) return stop(run, error_value(cmd::codes::failed, "the job was cancelled: nothing was written"));
			run.writes = !plan.empty();
			run.plan = std::make_shared<const pmg::migration_plan>(plan);
			// Nothing is written for an empty plan: the documents and the level stay as they are.
			if (run.writes) begin_writes(run);
			say_running(run, true);
			// The engine's loads of a desc hop to the main thread (this callback pumps them every frame); the rest runs on io.
			pmg::apply_context context{ run.root, m_deps.descs(), m_deps.resources(), {}, {}, {}, {}, &m_steps, {}, {} };
			context.on_main = [weak = std::weak_ptr<apply_handoff>(run.handoff)](const std::function<void()>& fn) {
				const auto handoff = weak.lock();
				return handoff && handoff->run_on_main(fn);
			};
			context.stop_requested = [weak = std::weak_ptr<apply_handoff>(run.handoff)](pmg::apply_checkpoint where) {
				const auto handoff = weak.lock();
				return !handoff || handoff->stop_requested(where);
			};
			run.handoff->hand([apply = m_deps.apply, plan = run.plan, context]() {
				apply_result result;
				try {
					result.outcome = apply ? apply(*plan, context) : pmg::apply_migration(*plan, context);
				} catch (const std::exception& e) {
					result.stopped = e.what();
				} catch (...) {
					result.stopped = "an unknown error";
				}
				return result;
			});
			run.applying = true;
			return true;
		}

		// rollback_migration writes files only (pmg_apply.cpp): no desc load, so no hop; the hand-over keeps the run's
		// cancel, stop and refusal the migration's.
		bool rollback_now(const std::shared_ptr<migration_run>& current)
		{
			migration_run& run = *current;
			show_progress(true);
			if (std::optional<json::value> refused = refusal_now(run)) return stop(run, std::move(*refused));
			const journal_view journal = read_journal(run.root, run.id);
			if (run.shown && rollback_fingerprint(run.id, journal) != *run.shown) {
				return stop(run, error_value(cmd::codes::denied, "the journal of the migration changed since the question: ask again"));
			}
			if (!journal.error.empty()) {
				return stop(run, json::object{ { "code", pmg::k_exit_refused }, { "status", pmg::journal_status(run.root, run.id) }, { "id", run.id },
					{ "refused", journal.error }, { "text", std::format("rollback of {}: {}", run.id, journal.error) } });
			}
			if (!run.handoff->claim()) return stop(run, error_value(cmd::codes::failed, "the job was cancelled: nothing was written"));
			run.journal_files = journal.restored;
			run.journal_files.insert(run.journal_files.end(), journal.created.begin(), journal.created.end());
			run.writes = true;
			begin_writes(run);
			say_running(run, true);
			// The io work rolls back only the journal checked here: one that reads otherwise by then is refused, unwritten.
			run.handoff->hand([rollback = m_deps.rollback, root = run.root, id = run.id, checked = pmg::journal_hash(journal.bytes),
								  weak = std::weak_ptr<apply_handoff>(run.handoff)]() {
				apply_result result;
				// As the commit of a migration: a stop seen now writes nothing; from here on a cancel is refused.
				const auto handoff = weak.lock();
				if (!handoff || handoff->stop_requested(pmg::apply_checkpoint::commit)) {
					result.not_run = true;
					return result;
				}
				try {
					pmg::rollback_context context{};
					context.wrote = &result.rollback_wrote;
					context.journal_hash = checked;
					result.rollback_error = rollback ? rollback(root, id, context) : pmg::rollback_migration(root, id, context);
				} catch (const std::exception& e) {
					result.stopped = e.what();
				} catch (...) {
					result.stopped = "an unknown error";
				}
				return result;
			});
			run.applying = true;
			return true;
		}

		// The main thread, once the rollback's io work handed its result back.
		bool after_rollback(const std::shared_ptr<migration_run>& current, apply_result result)
		{
			migration_run& run = *current;
			int code = pmg::k_exit_done;
			std::string text = std::format("migration {} rolled back", run.id);
			std::optional<std::string> refused;
			std::optional<std::string> error;
			if (result.not_run) {
				code = pmg::k_exit_refused;
				refused = "the job was cancelled before the rollback began: nothing was written";
			} else if (result.stopped) {
				// Its journal says whether it began: a migration not left unfinished by it had nothing written, and the level
				// comes back as after a refusal.
				const std::vector<std::string> left = pmg::unfinished_migrations(run.root);
				if (result.rollback_wrote || std::ranges::find(left, run.id) != left.end()) {
					code = pmg::k_exit_rollback_failed;
					error = std::format("the rollback stopped: {}; the state of the project is unknown: run editor --migrate-rollback {}", *result.stopped,
						run.id);
				} else {
					code = pmg::k_exit_refused;
					refused = std::format("the rollback stopped before it wrote anything: {}", *result.stopped);
				}
			} else if (result.rollback_error) {
				code = pmg::rollback_exit_code(result.rollback_error, result.rollback_wrote);
				if (code == pmg::k_exit_rollback_failed) error = *result.rollback_error;
				else refused = *result.rollback_error;
			}
			if (code != pmg::k_exit_done) text = std::format("rollback of {}: {}", run.id, error ? *error : *refused);
			const std::string status = pmg::journal_status(run.root, run.id);
			run.answer = json::object{ { "code", code }, { "status", status }, { "id", run.id }, { "text", text } };
			if (refused) run.answer["refused"] = *refused;
			if (error) run.answer["error"] = *error;
			if (code == pmg::k_exit_done) egLOG_INFO("project/migrate", "{}", text);
			else egLOG_WARN("project/migrate", "{}", text);

			// A refused rollback wrote nothing (rollback_migration refuses before its first write).
			const bool written = !refused.has_value();
			if (written) announce_changes(run.root, run.journal_files);
			run.hold.release();
			if (run.handoff->cancel_ignored()) {
				run.answer["cancel_ignored"] = true;
				run.answer["text"] = text + "\nThe cancel arrived after the rollback began: it was not cancelled.";
			}
			if (written && m_deps.rebuild_assets) m_deps.rebuild_assets();
			if (code == pmg::k_exit_rollback_failed) {
				// Put back only in part: as a migration that stopped, the level stays detached and unsaved until a rollback
				// of this migration finishes.
				const std::string reason = std::format("the rollback of migration {} did not finish: the level is not loaded from disk again; "
					"run editor --migrate-rollback {} first", run.id, run.id);
				if (m_deps.level_left_detached) {
					m_deps.level_left_detached(run.level, [root = run.root, id = run.id] {
						const std::vector<std::string> still = pmg::unfinished_migrations(root);
						return std::ranges::find(still, id) == still.end();
					}, reason);
				}
				run.answer["level"] = level_json(run, false, reason);
				return stop(run, std::move(run.answer));
			}
			if (code == pmg::k_exit_done && m_deps.migration_rolled_back) m_deps.migration_rolled_back(run.id);
			return reopen(current);
		}

		// Attaches the level again and starts its load; the run ends once the load ended (step, finish).
		bool reopen(const std::shared_ptr<migration_run>& current)
		{
			migration_run& run = *current;
			if (m_deps.reopen_level) {
				run.reopen_error = m_deps.reopen_level(run.level, [weak_run = std::weak_ptr<migration_run>(current)](bool loaded) {
					if (const auto ended = weak_run.lock()) {
						ended->load_ended = true;
						ended->loaded = loaded;
					}
				});
				run.load_started = run.level.is_valid() && !run.reopen_error;
			}
			return true;
		}

		// The main thread, once the io work handed the result back: what S7 did right after apply_migration.
		bool after_apply(const std::shared_ptr<migration_run>& current, apply_result result)
		{
			migration_run& run = *current;
			if (run.rollback) return after_rollback(current, std::move(result));
			const pmg::migration_plan& plan = *run.plan;
			const bool writes = run.writes;
			const pmg::apply_outcome& outcome = result.outcome;
			const std::optional<std::string>& stopped = result.stopped;

			if (stopped) {
				// What was written is not known: as a rollback that did not finish.
				const std::vector<std::string> left = pmg::unfinished_migrations(run.root);
				std::string error = std::format("the migration stopped: {}; the state of the project is unknown", *stopped);
				if (!left.empty()) error += std::format(": run editor --migrate-rollback {}", left.front());
				run.answer = json::object{ { "code", pmg::k_exit_rollback_failed }, { "status", pmg::to_string(pmg::migration_status::rollback_failed) },
					{ "error", error }, { "text", pmg::plan_text(plan) } };
				if (!left.empty()) run.answer["unfinished"] = strings_of(left);
				egLOG_ERROR("project/migrate", "{}", error);
				// Which of them were written is not known: every file the plan names is announced.
				std::vector<std::string> planned;
				planned.reserve(plan.files.size());
				for (const pmg::file_plan& file : plan.files) planned.push_back(file.path);
				announce_changes(run.root, planned);
			} else if (!outcome.refused.empty()) {
				run.answer = json::object{ { "code", pmg::k_exit_refused }, { "refused", outcome.refused }, { "text", pmg::plan_text(plan) } };
				egLOG_WARN("project/migrate", "refused: {}", outcome.refused);
			} else {
				const pmg::migration_report& report = outcome.report;
				run.answer = json::object{ { "code", pmg::exit_code_of(report.status) }, { "status", pmg::to_string(report.status) },
					{ "id", report.id }, { "text", report.id.empty() ? pmg::plan_text(plan) : pmg::report_text(report) } };
				run.answer["report"] = pmg::report_to_json(report);
				egLOG_INFO("project/migrate", "migration {} of {}: {}", report.id, fs_paths::generic_utf8_of(run.root), pmg::to_string(report.status));
				std::vector<std::string> written;
				written.reserve(report.files.size() + report.created_files.size());
				for (const pmg::applied_file& file : report.files) written.push_back(file.path);
				written.insert(written.end(), report.created_files.begin(), report.created_files.end());
				announce_changes(run.root, written);
			}
			// Announced under the hold: a file the mounts reported meanwhile is told once (signal_changed drops its held
			// signal); the rest of what they reported goes now, coalesced.
			run.hold.release();
			if (run.handoff->cancel_ignored()) {
				run.answer["cancel_ignored"] = true;
				const json::value* text = run.answer.if_contains("text");
				run.answer["text"] = std::string(text && text->is_string() ? std::string_view(text->as_string()) : std::string_view{})
					+ "\nThe cancel arrived after the migration was committed: the migration was not cancelled.";
			}

			if (!writes) {
				run.answer["level"] = level_json(run, false, "nothing was migrated: the level stayed open");
				return stop(run, std::move(run.answer));
			}
			if (m_deps.rebuild_assets) m_deps.rebuild_assets();
			if (stopped) {
				// The files on disk may be half migrated: the level stays detached, so neither a load nor a save of the old level
				// touches them before the rollback.
				const std::vector<std::string> left = pmg::unfinished_migrations(run.root);
				const std::string reason = left.empty()
					? std::string("the migration stopped: the level is not loaded from disk again")
					: std::format("the migration stopped: the level is not loaded from disk again; run editor --migrate-rollback {} first", left.front());
				if (m_deps.level_left_detached) {
					// Rolled back once the journal of this migration is no longer unfinished; without one there is nothing to roll back.
					std::function<bool()> rolled_back = [root = run.root, id = left.empty() ? std::string{} : left.front()] {
						if (id.empty()) return false;
						const std::vector<std::string> still = pmg::unfinished_migrations(root);
						return std::ranges::find(still, id) == still.end();
					};
					m_deps.level_left_detached(run.level, std::move(rolled_back), reason);
				}
				run.answer["level"] = level_json(run, false, reason);
				return stop(run, std::move(run.answer));
			}
			return reopen(current);
		}

		void announce_changes(const fs::path& root, const std::vector<std::string>& paths) const
		{
			if (!m_deps.signal_changed) return;
			const std::vector<fs::path> roots = m_deps.content_roots ? m_deps.content_roots() : std::vector<fs::path>{};
			for (const std::string& path : paths) {
				if (const std::optional<res::tag> tag = tag_below_content_root(root, roots, path)) m_deps.signal_changed(*tag);
			}
		}

		static json::object level_json(const migration_run& run, bool reopened, std::string_view reason)
		{
			json::object out{ { "tag", std::string(run.level.view()) }, { "reopened", reopened } };
			if (!reopened) out["reason"] = reason;
			return out;
		}

		void finish(migration_run& run)
		{
			if (!run.level.is_valid()) {
				run.answer["level"] = level_json(run, false, "no level was open");
			} else if (run.reopen_error) {
				run.answer["level"] = level_json(run, false, *run.reopen_error);
			} else {
				const bool reopened = run.load_ended ? run.loaded : (m_deps.current_level_tag && m_deps.current_level_tag() == run.level);
				run.answer["level"] = level_json(run, reopened, "the level did not load again from disk (see the log and Problems)");
			}
			run.fulfil(std::move(run.answer));
			end_run(run);
		}

		edt::migration_deps m_deps;
		pmg::project_step_registry m_steps;
		std::vector<shown_plan> m_shown;
		std::shared_ptr<migration_run> m_running;
		// Last: destroyed first, so the job is stopped before the rest goes.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};
}

std::vector<edt::registration> edt::register_migration_commands(cmd::command_registry& commands, migration_deps deps)
{
	auto impl = std::make_shared<migration_commands>(std::move(deps));
	std::vector<registration> out;

	cmd::command_desc plan;
	plan.id = "project.migration_plan";
	plan.summary = "The plan of migrating the open project's data to this engine (as editor --migrate --dry-run): files, steps, "
		"what is not migrated and the exit code of the dry run. Writes nothing";
	plan.args_schema = parse_schema(k_no_args_schema);
	plan.result_schema = parse_schema(k_plan_result_schema);
	plan.category = cmd::agent_category::read;
	plan.invoke = [impl](const cmd::call& call) { return impl->plan(call); };
	if (registration added = commands.add(std::move(plan)); added.active()) out.push_back(std::move(added));

	cmd::command_desc migrate;
	migrate.id = "project.migrate";
	migrate.summary = "Migrate the open project in place after a person approved the plan they were shown (as editor --migrate, "
		"without --force). Refused while Play runs, a level loads or a build runs (`busy`) and with unsaved changes (`needs_input`). "
		"Open documents are closed and the level is opened again; the job's result is the report";
	migrate.args_schema = parse_schema(k_migrate_args_schema);
	migrate.result_schema = parse_schema(k_job_result_schema);
	migrate.category = cmd::agent_category::project_migrate;
	migrate.risk = cmd::danger::confirm;
	migrate.mutates = true;
	migrate.destructive = false;
	migrate.starts_job = true;
	migrate.invoke = [impl](const cmd::call& call) { return impl->migrate(call); };
	migrate.describe = [impl](const cmd::call& call) { return impl->describe(call); };
	// The plan is recorded on every path that runs it, the --agent-policy answer too.
	migrate.describe_before_allowed_invoke = true;
	if (registration added = commands.add(std::move(migrate)); added.active()) out.push_back(std::move(added));

	cmd::command_desc rollback;
	rollback.id = "project.migrate_rollback";
	rollback.summary = "Roll back a migration of the open project from its journal and backup in .snk/migrations/<id> (as editor "
		"--migrate-rollback <id>) after a person approved it. Refused while Play runs, a level loads or a build runs (`busy`) and with "
		"unsaved changes (`needs_input`). Open documents are closed and the level is opened again; the job's result is "
		"{code, status, id, text}: code 0 rolled back, 2 refused (nothing written), 6 put back only in part";
	rollback.args_schema = parse_schema(k_rollback_args_schema);
	rollback.result_schema = parse_schema(k_job_result_schema);
	rollback.category = cmd::agent_category::project_migrate;
	rollback.risk = cmd::danger::confirm;
	rollback.mutates = true;
	rollback.destructive = false;
	rollback.starts_job = true;
	rollback.invoke = [impl](const cmd::call& call) { return impl->rollback(call); };
	rollback.describe = [impl](const cmd::call& call) { return impl->describe_rollback(call); };
	rollback.describe_before_allowed_invoke = true;
	if (registration added = commands.add(std::move(rollback)); added.active()) out.push_back(std::move(added));
	return out;
}
