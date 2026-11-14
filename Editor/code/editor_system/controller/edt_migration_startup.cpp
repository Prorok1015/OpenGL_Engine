#include "edt_migration_startup.h"
#include "edt_migration_commands.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <utility>

namespace {
	namespace json = boost::json;
	namespace pmg = prj::migration;

	// FNV-1a 64 of the parts, each with its size first.
	class set_hash
	{
	public:
		void add(std::string_view part)
		{
			const std::string sized = std::format("{}:", part.size());
			mix(sized);
			mix(part);
		}

		std::string text() const { return std::format("{:016x}", m_hash); }

	private:
		void mix(std::string_view bytes)
		{
			for (const char c : bytes) {
				m_hash ^= static_cast<unsigned char>(c);
				m_hash *= 0x100000001b3ull;
			}
		}

		std::uint64_t m_hash = 0xcbf29ce484222325ull;
	};
	// A journal's folder name is whatever is on the disk: shown without control characters and without '#' (a dialog's title
	// is its ImGui name, where "##" and "###" change the ID), cut short.
	std::string shown_id(std::string_view id)
	{
		std::string out;
		out.reserve(id.size() < 128 ? id.size() : 128);
		for (const char c : id) {
			if (out.size() >= 128) {
				out += "...";
				break;
			}
			const auto byte = static_cast<unsigned char>(c);
			out.push_back(byte < 0x20 || byte == 0x7f || c == '#' ? '?' : c);
		}
		return out;
	}
}

edt::journal_offer edt::decide_journal_offer(const std::vector<std::string>& unfinished, bool headless)
{
	journal_offer offer;
	if (unfinished.empty()) return offer;
	offer.id = unfinished.front();
	const std::string id = shown_id(offer.id);
	const std::string others = unfinished.size() > 1 ? std::format(" ({} more after it)", unfinished.size() - 1) : std::string{};
	if (!valid_migration_id(offer.id)) {
		// Not a name apply_migration makes: neither the editor nor the command line rolls it back.
		const std::string reason = std::format("the folder .snk/migrations/{} holds an unfinished journal, but its name is not a "
			"migration id, so it cannot be rolled back from the editor; look at it and restore its backup by hand{}", id, others);
		offer.what = headless ? journal_offer::kind::warn : journal_offer::kind::inform;
		offer.title = "An unfinished migration";
		offer.text = reason;
		return offer;
	}
	if (headless) {
		offer.what = journal_offer::kind::warn;
		offer.text = std::format("migration {} did not finish{}: the project may be partly migrated; roll it back with "
			"editor --headless --project <dir> --migrate-rollback {}", id, others, id);
		return offer;
	}
	offer.what = journal_offer::kind::ask;
	offer.title = std::format("Migration {} did not finish", id);
	offer.text = std::format("The migration {} of this project did not finish{}: the project may be partly migrated.\n"
		"Roll it back now? Every file it wrote is put back from .snk/migrations/{}/backup.\n"
		"Later: Project > Migrate Project... offers it again, or run editor --migrate-rollback {}.", id, others, id, id);
	return offer;
}

edt::migration_startup::migration_startup(ports ports, bool headless) : m_ports(std::move(ports)), m_headless(headless)
{
}

bool edt::migration_startup::tick()
{
	if (m_done) return false;
	const auto unfinished = [this] { return m_ports.unfinished ? m_ports.unfinished() : std::vector<std::string>{}; };
	if (m_headless) {
		m_done = true;
		const journal_offer offer = decide_journal_offer(unfinished(), true);
		if (offer.what == journal_offer::kind::warn && m_ports.warn) m_ports.warn(offer.text);
		return false;
	}
	if (m_ports.settled && !m_ports.settled()) return true;
	m_done = true;
	const journal_offer offer = decide_journal_offer(unfinished(), false);
	if (offer.what != journal_offer::kind::none) {
		if (m_ports.warn) m_ports.warn(offer.text);
		if (m_ports.ask) m_ports.ask(offer);
	}
	if (m_ports.start_scan) m_ports.start_scan();
	return false;
}

std::filesystem::path edt::migration_notice_file(const std::vector<std::filesystem::path>& edt_roots, const std::filesystem::path& user_data_dir)
{
	return (edt_roots.empty() ? user_data_dir : edt_roots.back()) / "settings" / "migration_notice.json";
}

edt::migration_scan_summary edt::summarize_scan(const pmg::scan_result& scan, const pmg::project_step_registry& steps)
{
	migration_scan_summary out;
	set_hash hash;
	// The order of plan_migration: the files as the scan sorted them, the pre steps, then the post ones.
	for (const pmg::scanned_file& file : scan.files) {
		if (file.kind != pmg::file_class::outdated) continue;
		++out.outdated;
		hash.add(file.path);
	}
	for (const pmg::project_phase phase : { pmg::project_phase::pre, pmg::project_phase::post }) {
		for (const pmg::project_step& step : steps.steps()) {
			if (step.phase != phase || !step.needed || !step.needed(scan)) continue;
			out.steps.push_back(step.id);
			hash.add("step:" + step.id);
		}
	}
	out.manifest = scan.manifest.kind == pmg::file_class::outdated && scan.count(pmg::file_class::unknown_type) == 0;
	out.complete = !scan.incomplete;
	hash.add(out.manifest ? "manifest" : "");
	out.set = hash.text();
	return out;
}

edt::migration_notice_model::migration_notice_model(ports ports) : m_ports(std::move(ports))
{
}

bool edt::migration_notice_model::deliver(const scan_ticket& started, const scan_ticket& now, migration_scan_summary summary)
{
	if (!(started == now)) {
		egLOG_INFO("project/migrate", "the format check of the project is dropped: the project, the game layer or its files (a migration "
			"or a rollback) changed while it ran");
		return false;
	}
	if (!summary.complete) {
		egLOG_INFO("project/migrate", "the format check of the project was stopped before its end: nothing is shown");
		return false;
	}
	m_ticket = started;
	m_summary = std::move(summary);
	if (!m_summary->needed()) return true;
	const std::optional<std::string> text = m_ports.load ? m_ports.load() : std::nullopt;
	const std::vector<later_entry> entries = text ? parse_later(*text) : std::vector<later_entry>{};
	m_hidden = std::ranges::any_of(entries, [this](const later_entry& entry) {
		return entry.project == m_ticket->project && entry.set == m_summary->set;
	});
	return true;
}

std::string edt::migration_notice_model::text() const
{
	if (!m_summary) return {};
	if (m_summary->outdated > 0) {
		return std::format("{} file{} of this project {} in an old format.", m_summary->outdated, m_summary->outdated == 1 ? "" : "s",
			m_summary->outdated == 1 ? "is" : "are");
	}
	return "This project needs a migration to this engine.";
}

std::optional<std::string> edt::migration_notice_model::later()
{
	m_hidden = true;
	if (!m_ticket || !m_summary) return std::nullopt;
	const std::optional<std::string> text = m_ports.load ? m_ports.load() : std::nullopt;
	std::vector<later_entry> entries = text ? parse_later(*text) : std::vector<later_entry>{};
	std::erase_if(entries, [this](const later_entry& entry) { return entry.project == m_ticket->project; });
	entries.push_back({ m_ticket->project, m_summary->set });
	if (entries.size() > k_max_remembered) entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(k_max_remembered));
	if (!m_ports.save) return std::string("the settings cannot be written");
	std::optional<std::string> error = m_ports.save(serialize_later(entries));
	if (error) egLOG_WARN("project/migrate", "the migration notice's Later is not kept: {}", *error);
	return error;
}

std::vector<edt::migration_notice_model::later_entry> edt::migration_notice_model::parse_later(std::string_view text)
{
	std::vector<later_entry> out;
	boost::system::error_code error;
	const json::value root = json::parse(text, error);
	if (error || !root.is_object()) return out;
	const json::value* later = root.as_object().if_contains("later");
	if (!later || !later->is_array()) return out;
	for (const json::value& item : later->as_array()) {
		if (!item.is_object()) continue;
		const json::value* project = item.as_object().if_contains("project");
		const json::value* set = item.as_object().if_contains("set");
		if (!project || !project->is_string() || !set || !set->is_string()) continue;
		out.push_back({ std::string(project->as_string()), std::string(set->as_string()) });
	}
	if (out.size() > k_max_remembered) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(k_max_remembered));
	return out;
}

std::string edt::migration_notice_model::serialize_later(const std::vector<later_entry>& entries)
{
	json::array later;
	for (const later_entry& entry : entries) later.push_back(json::object{ { "project", entry.project }, { "set", entry.set } });
	json::object root;
	root["later"] = std::move(later);
	return prj::to_indented_json(root);
}
