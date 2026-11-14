#pragma once
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/project_migration/pmg_scan.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What the editor does about migrations a project was left with (EPIC-58 S8, #761) once the project is open and its
	// start level settled: a person is asked whether to roll the first one back now; headless nobody is asked, a WARN names
	// the command line that does it. No ImGui.
	struct journal_offer
	{
		enum class kind : std::uint8_t { none, ask, warn };

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

		// The project (its canonical path) and the code (migration_deps::code_identity) a scan was made for.
		struct scan_ticket
		{
			std::string project;
			std::uint64_t code_identity = 0;

			bool operator==(const scan_ticket&) const = default;
		};

		// At most this many projects are remembered in the settings; the oldest goes first.
		static constexpr std::size_t k_max_remembered = 64;

		explicit migration_notice_model(ports ports);

		// The scan made for `started` ended; `now` is the project and the code as they are. False: the result is dropped
		// (another project, or the game layer changed meanwhile, so the scan's types are not the editor's any more).
		bool deliver(const scan_ticket& started, const scan_ticket& now, migration_scan_summary summary);
		bool visible() const { return m_summary && m_summary->needed() && !m_hidden; }
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
