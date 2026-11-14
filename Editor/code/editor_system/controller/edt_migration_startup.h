#pragma once
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/project_migration/pmg_scan.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What the editor does about migrations a project was left with (EPIC-58 S8, #761) once the project is open and its
	// start level settled: a person is asked whether to roll the first one back now; headless nobody is asked, a WARN names
	// the command line that does it. A folder name that is no migration id (valid_migration_id) is not offered for a
	// rollback: the person is told why (`inform`). No ImGui.
	struct journal_offer
	{
		enum class kind : std::uint8_t { none, ask, inform, warn };

		kind what = kind::none;
		// The migration to roll back: the first unfinished one (they are sorted by id, so the oldest).
		std::string id;
		std::string title;
		std::string text;
	};

	inline constexpr std::string_view k_journal_offer_roll_back = "Roll Back";
	inline constexpr std::string_view k_journal_offer_later = "Later";

	// `unfinished`: prj::migration::unfinished_migrations of the project's root.
	journal_offer decide_journal_offer(const std::vector<std::string>& unfinished, bool headless);

	// The save block of a migration (or a rollback) that stopped half way (#761): it holds for the whole project, whatever
	// level is loaded meanwhile, until `rolled_back()` says that migration was rolled back. No ImGui.
	class migration_save_block
	{
	public:
		void hold(std::function<bool()> rolled_back, std::string reason)
		{
			m_rolled_back = std::move(rolled_back);
			m_reason = std::move(reason);
		}

		// Why nothing may be saved; nullopt once the migration was rolled back (the block goes then) or none was held.
		std::optional<std::string> reason()
		{
			if (!m_reason) return std::nullopt;
			if (m_rolled_back && m_rolled_back()) {
				m_reason.reset();
				m_rolled_back = {};
				return std::nullopt;
			}
			return m_reason;
		}

	private:
		std::function<bool()> m_rolled_back;
		std::optional<std::string> m_reason;
	};

	// The checks of an opened project, each once (#761): the rollback question, then the background scan. Headless: a WARN
	// only, no question, no scan. No ImGui.
	class migration_startup
	{
	public:
		struct ports
		{
			// The start level settled: nothing loads any more.
			std::function<bool()> settled;
			// prj::migration::unfinished_migrations of the project.
			std::function<std::vector<std::string>()> unfinished;
			// Asks the person (an `ask` or an `inform` offer).
			std::function<void(const journal_offer& offer)> ask;
			std::function<void(const std::string& text)> warn;
			std::function<void()> start_scan;
		};

		migration_startup(ports ports, bool headless);

		// Every frame until it returns false.
		bool tick();

	private:
		ports m_ports;
		bool m_headless = false;
		bool m_done = false;
	};

	// Where the notice's Later is kept: the person's settings (the last edt:// root, else `user_data_dir`), never the project.
	std::filesystem::path migration_notice_file(const std::vector<std::filesystem::path>& edt_roots, const std::filesystem::path& user_data_dir);

	// What a scan of the project found that a migration would change (#761), from the scan alone (it runs on a worker
	// thread, without the game layer): what makes project.migration_plan answer code 1.
	struct migration_scan_summary
	{
		// Files of an older format: outdated descs and .resource sidecars.
		std::size_t outdated = 0;
		// The project steps whose `needed` holds for the scan, in the registry's order.
		std::vector<std::string> steps;
		// project.snkproj names an older engine and no file of an unloaded module is left: the migration stamps it.
		bool manifest = false;
		// The set the scan found (the outdated paths, the steps, the manifest): FNV-1a, 16 hex digits.
		std::string set;
		// The scan saw the whole project (it was not stopped).
		bool complete = true;
		// Migrations left unfinished: their rollback question goes first, the notice waits.
		std::vector<std::string> unfinished;

		bool needed() const { return outdated > 0 || !steps.empty() || manifest; }
	};

	migration_scan_summary summarize_scan(const prj::migration::scan_result& scan, const prj::migration::project_step_registry& steps);

	// The notice "N files in an old format" (#761): shown once a scan of the open project found a migration is needed,
	// until the person opens the window or says Later. Later is kept in the person's own settings
	// (edt://settings/migration_notice.json), per project and per set the scan found, never in the project: another set
	// shows the notice again. No ImGui.
	class migration_notice_model
	{
	public:
		struct ports
		{
			// The settings file's text; nullopt: there is none (or it cannot be read).
			std::function<std::optional<std::string>()> load;
			// Writes the settings file; the error, nullopt on success.
			std::function<std::optional<std::string>(const std::string& text)> save;
		};

		// The project (its canonical path), the code (migration_deps::code_identity) and the number of migration and
		// rollback runs so far that a scan was made for: a run that started since makes the scan stale.
		struct scan_ticket
		{
			std::string project;
			std::uint64_t code_identity = 0;
			std::uint64_t migration_runs = 0;

			bool operator==(const scan_ticket&) const = default;
		};

		// At most this many projects are remembered in the settings; the oldest goes first.
		static constexpr std::size_t k_max_remembered = 64;

		explicit migration_notice_model(ports ports);

		// The scan made for `started` ended; `now` is the project, the code and the runs as they are. False: the result is
		// dropped (another project, the game layer changed, a migration or rollback ran meanwhile, or the scan was stopped).
		bool deliver(const scan_ticket& started, const scan_ticket& now, migration_scan_summary summary);
		bool visible() const { return m_summary && m_summary->needed() && m_summary->unfinished.empty() && !m_hidden; }
		const std::optional<migration_scan_summary>& summary() const { return m_summary; }
		std::string text() const;
		// Open Migration...: the notice goes for this session.
		void open() { m_hidden = true; }
		// A migration ran or runs: the notice goes for this session.
		void hide() { m_hidden = true; }
		// Later: kept per project and set; the error of the write, nullopt when written.
		std::optional<std::string> later();

		// One Later of the settings file.
		struct later_entry
		{
			std::string project;
			std::string set;
		};

		// The settings file: {"later": [{"project": "<canonical path>", "set": "<hex>"}...]}; what does not read is dropped.
		static std::vector<later_entry> parse_later(std::string_view text);
		static std::string serialize_later(const std::vector<later_entry>& entries);

	private:
		ports m_ports;
		std::optional<scan_ticket> m_ticket;
		std::optional<migration_scan_summary> m_summary;
		bool m_hidden = false;
	};
}
