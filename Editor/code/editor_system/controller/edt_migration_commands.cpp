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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <future>
#include <iterator>
#include <memory>
#include <utility>

namespace {
	namespace json = boost::json;
	namespace fs = std::filesystem;
	namespace pmg = prj::migration;

	constexpr std::size_t k_max_shown = 32;
	// What a person was shown lives as long as the question about it.
	constexpr auto k_shown_lifetime = cmd::command_registry::confirmation_lifetime;
	// How often the job's thread looks at its stop token while it waits for the main thread.
	constexpr std::chrono::milliseconds k_wait_step{ 50 };
	constexpr std::size_t k_max_listed = 5;

	constexpr std::string_view k_no_args_schema = R"json({"type":"object","properties":{},"additionalProperties":false})json";

	constexpr std::string_view k_plan_result_schema = R"json({"type":"object","properties":{
		"code":{"type":"integer","description":"what editor --migrate --dry-run exits with: 0 nothing to do, 1 a migration is needed, 2 refused, 4 only files that are never migrated"},
		"plan":{"type":"object"},
		"text":{"type":"string"}},
		"required":["code","plan","text"]})json";

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

	// The plan as the answer gives it (every file's steps and warnings, the project steps, what is skipped, the manifest)
	// and every byte of the project it was made from.
	std::uint64_t fingerprint_of(const fs::path& root, const pmg::migration_plan& plan)
	{
		const std::uintmax_t max_bytes = pmg::scan_options{}.max_file_bytes;
		fnv1a_hash hash;
		hash.add_part(json::serialize(plan_json(plan)));
		hash.add_file(root, fs_paths::path_from_utf8(prj::k_manifest_file_name), max_bytes, true);
		std::vector<std::pair<std::string, std::string_view>> files;
		files.reserve(plan.scan.files.size());
		for (const pmg::scanned_file& file : plan.scan.files) files.emplace_back(file.path, pmg::to_string(file.kind));
		std::ranges::sort(files);
		for (const auto& [path, kind] : files) {
			hash.add_part(path);
			hash.add_part(kind);
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

	// One project.migrate from the job's start to its answer; the steps run in the implicit frame callback.
	struct migration_run
	{
		std::promise<json::value> promise;
		// Set by the job when it was cancelled: a run that has not written yet stops without writing.
		std::shared_ptr<std::atomic<bool>> abandoned = std::make_shared<std::atomic<bool>>(false);
		fs::path root;
		// What the person was shown; none for a person's own call.
		std::optional<std::uint64_t> shown;
		int frame = 0;
		res::tag level;
		bool modal_closed = false;
		bool load_started = false;
		bool load_ended = false;
		bool loaded = false;
		std::optional<std::string> reopen_error;
		json::object answer;
		bool fulfilled = false;

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
			return cmd::result::success(json::object{ { "code", code }, { "plan", std::move(planned) }, { "text", pmg::plan_text(plan) } });
		}

		std::optional<std::string> describe(const cmd::call& call)
		{
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root) return std::string("This request will be refused: no project is open");
			const pmg::migration_plan plan = make_plan(*root);
			remember(key_of(call.from), call.confirmation, fingerprint_of(*root, plan));

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
			shown_plan* shown = person ? nullptr : find_shown(key, call.confirmation);
			if (!person && !shown) return cmd::result::failure(cmd::codes::denied, "the plan was not shown to a person; ask again");
			// Refused for now: a retry (agent_confirmations repeats a `busy` with a fresh ticket, whose description sees the
			// project as it is then) is still judged by the plan the person approved.
			const auto refuse = [shown](std::string code, std::string message) {
				if (shown) shown->approved = true;
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
			}
			auto work = [future = run->promise.get_future().share(), abandoned = run->abandoned](ds::task_context& task) -> json::value {
				try {
					while (future.wait_for(k_wait_step) != std::future_status::ready) {
						if (task.stop.stop_requested()) {
							abandoned->store(true);
							throw ds::task_cancelled{};
						}
					}
					return future.get();
				} catch (const std::future_error&) {
					return error_value(cmd::codes::failed, "the editor stopped before the migration ended");
				}
			};
			const std::uint64_t job = m_deps.jobs.start_with_result(ds::lane::io, ds::task_desc{ "Migrate project", "project", true }, m_lifetime, std::move(work));
			m_running = run;
			egLOG_INFO("project/migrate", "migration of {} started (job {})", fs_paths::generic_utf8_of(*root), job);
			m_deps.register_implicit(edt::k_migration_frame_id, [weak = weak_from_this(), run] {
				const auto self = weak.lock();
				return self && self->step(run);
			});
			return cmd::result::success(json::object{ { "job", static_cast<std::int64_t>(job) } });
		}

	private:
		static constexpr const char* k_no_project = "no_project";

		// A plan a person was shown, by caller and by the ticket its question got (empty: described for a call allowed
		// without a question). `approved`: a run the person approved was refused for now (busy): this record judges the
		// retries, and a later question of the caller does not replace it.
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

		void forget_expired()
		{
			const auto at = now();
			std::erase_if(m_shown, [&](const shown_plan& shown) { return at - shown.at > k_shown_lifetime; });
		}

		// A new question of the caller replaces its unanswered one, never an approved one.
		void remember(std::string key, std::string ticket, std::uint64_t value)
		{
			forget_expired();
			std::erase_if(m_shown, [&](const shown_plan& shown) { return shown.key == key && !shown.approved; });
			if (m_shown.size() >= k_max_shown) m_shown.erase(m_shown.begin());
			m_shown.push_back({ std::move(key), std::move(ticket), value, now(), false });
		}

		// The approved record of the caller first; else the one described for this very ticket.
		shown_plan* find_shown(const std::string& key, const std::string& ticket)
		{
			forget_expired();
			const auto approved = std::ranges::find_if(m_shown, [&](const shown_plan& shown) { return shown.key == key && shown.approved; });
			if (approved != m_shown.end()) return &*approved;
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

		// Ends a run that loads no level again.
		bool stop(migration_run& run, json::value answer)
		{
			show_progress(false);
			run.fulfil(std::move(answer));
			m_running.reset();
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
			if (run.frame == 2) return migrate_now(current);
			if (!run.modal_closed) {
				show_progress(false);
				run.modal_closed = true;
			}
			const bool loading = m_deps.level_load_in_progress && m_deps.level_load_in_progress();
			if (run.load_started && !run.load_ended && loading) return true;
			finish(run);
			return false;
		}

		bool migrate_now(const std::shared_ptr<migration_run>& current)
		{
			migration_run& run = *current;
			show_progress(true);
			if (run.abandoned->load()) return stop(run, error_value(cmd::codes::failed, "the job was cancelled: nothing was written"));
			const std::optional<fs::path> root = m_deps.project_root ? m_deps.project_root() : std::nullopt;
			if (!root || *root != run.root) return stop(run, error_value(k_no_project, "the project is not open any more: nothing was written"));
			if (auto why = m_deps.busy_reason ? m_deps.busy_reason() : std::nullopt) return stop(run, error_value(cmd::codes::busy, *why));
			if (auto why = m_deps.unsaved_reason ? m_deps.unsaved_reason() : std::nullopt) return stop(run, error_value(cmd::codes::needs_input, *why));

			const pmg::migration_plan plan = make_plan(run.root);
			if (run.shown && fingerprint_of(run.root, plan) != *run.shown) {
				return stop(run, error_value(cmd::codes::denied, "the project changed since the question: ask again"));
			}
			const std::vector<std::string> unfinished = pmg::unfinished_migrations(run.root);
			const std::string refused = !unfinished.empty() ? unfinished_refusal(unfinished) : pmg::plan_refusal(plan);
			if (!refused.empty()) {
				json::object answer{ { "code", pmg::k_exit_refused }, { "refused", refused }, { "text", pmg::plan_text(plan) } };
				if (!unfinished.empty()) answer["unfinished"] = strings_of(unfinished);
				return stop(run, std::move(answer));
			}

			const bool writes = !plan.empty();
			// Nothing is written for an empty plan: the documents and the level stay as they are.
			if (writes) run.level = m_deps.close_documents_and_detach_level ? m_deps.close_documents_and_detach_level() : res::tag{};
			pmg::apply_outcome outcome;
			std::optional<std::string> stopped;
			try {
				const pmg::apply_context context{ run.root, m_deps.descs(), m_deps.resources(), m_deps.pause_watch, {}, {}, {}, &m_steps };
				outcome = m_deps.apply ? m_deps.apply(plan, context) : pmg::apply_migration(plan, context);
			} catch (const std::exception& e) {
				stopped = e.what();
			}

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

			if (!writes) {
				run.answer["level"] = level_json(run, false, "nothing was migrated: the level stayed open");
				return stop(run, std::move(run.answer));
			}
			if (m_deps.rebuild_assets) m_deps.rebuild_assets();
			if (stopped) {
				// The files on disk may be half migrated: the level stays detached, so neither a load nor a save of the old level
				// touches them before the rollback.
				const std::vector<std::string> left = pmg::unfinished_migrations(run.root);
				run.answer["level"] = level_json(run, false, left.empty()
					? std::string("the migration stopped: the level is not loaded from disk again")
					: std::format("the migration stopped: the level is not loaded from disk again; run editor --migrate-rollback {} first", left.front()));
				return stop(run, std::move(run.answer));
			}
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
			m_running.reset();
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
	migrate.args_schema = parse_schema(k_no_args_schema);
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
	return out;
}
