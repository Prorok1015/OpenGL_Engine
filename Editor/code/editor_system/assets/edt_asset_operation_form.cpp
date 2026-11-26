#include "edt_asset_operation_form.h"
#include "edt_forge_types.h"
#include <cstddef>
#include <format>
#include <set>
#include <utility>

namespace
{
	constexpr std::size_t k_listed_files = 12;
	constexpr std::size_t k_listed_pointers = 6;
	constexpr std::size_t k_listed_places = 12;
	constexpr std::size_t k_shown_bytes = 256;
	constexpr std::string_view k_protocol = "res://";

	std::string shown(std::string_view text)
	{
		return edt::forge::escaped_text(text, k_shown_bytes);
	}

	std::string relative_of(const res::tag& asset)
	{
		const std::string_view text = asset.view();
		return std::string(text.starts_with(k_protocol) ? text.substr(k_protocol.size()) : text);
	}

	void add_places(std::vector<std::string>& out, const std::vector<edt::asset_usage>& places, std::size_t limit)
	{
		for (std::size_t i = 0; i < places.size() && i < limit; ++i) out.push_back("  " + shown(edt::usage_row_text(places[i])));
		if (places.size() > limit) out.push_back(std::format("  ... and {} more", places.size() - limit));
	}

	void add_pointers(std::vector<std::string>& out, const std::vector<std::string>& pointers)
	{
		for (std::size_t i = 0; i < pointers.size() && i < k_listed_pointers; ++i) out.push_back("      " + shown(pointers[i]));
		if (pointers.size() > k_listed_pointers) out.push_back(std::format("      ... and {} more", pointers.size() - k_listed_pointers));
	}

	void add_failures(std::vector<std::string>& out, const std::vector<edt::rewrite_failure>& failures)
	{
		for (std::size_t i = 0; i < failures.size() && i < k_listed_files; ++i) {
			out.push_back(std::format("  {}  ({})", shown(failures[i].file), shown(failures[i].reason)));
		}
		if (failures.size() > k_listed_files) out.push_back(std::format("  ... and {} more file(s)", failures.size() - k_listed_files));
	}

	const char* sentence_of(edt::refusal_code code)
	{
		switch (code) {
		case edt::refusal_code::one_asset_only: return "Only one file can be changed at a time.";
		case edt::refusal_code::no_database: return "No project is open.";
		case edt::refusal_code::database_not_ready: return "The asset database is still reading the project's files: try again in a moment.";
		case edt::refusal_code::not_res_path: return "Only files under res:// can be changed.";
		case edt::refusal_code::not_found: return "The file is not in the project.";
		case edt::refusal_code::engine_layer: return "The file belongs to the engine and is never changed.";
		case edt::refusal_code::not_top_layer: return "The file is not in the project's own folder: it comes from a lower layer.";
		case edt::refusal_code::meta_in_other_layer: return "The file's .resource is in another layer than the file.";
		case edt::refusal_code::bad_name: return "That is not a name the project can hold.";
		case edt::refusal_code::no_such_folder: return "That folder does not exist in the project's own folder.";
		case edt::refusal_code::unportable_path: return "That path would not work on every platform.";
		case edt::refusal_code::no_change: return "The file has this name and folder already: change it.";
		case edt::refusal_code::case_only_rename: return "A change of letter case alone is not done: on some systems both names are the same file.";
		case edt::refusal_code::target_exists: return "A file is there already under that name.";
		case edt::refusal_code::binary_name_lost: return "The desc's binary file goes by the desc's own <name>.<kind>.desc name, which this name does not keep.";
		case edt::refusal_code::usage_scan_skipped: return "Some desc files are too big to be read, so who refers to the file is not known.";
		case edt::refusal_code::referrer_open: return "A document with unsaved changes is open: save or close it, then try again.";
		case edt::refusal_code::has_referrers: return "Other files refer to it.";
		case edt::refusal_code::stale_plan: return "A file changed after the preview: look at the preview again.";
		case edt::refusal_code::io_error: return "A file could not be read or moved.";
		case edt::refusal_code::binary_pair_blocked: return "The desc's binary file cannot go along with it.";
		case edt::refusal_code::unrewritable_references: return "Some files that refer to it cannot be rewritten, so its references would break: nothing was changed.";
		case edt::refusal_code::mixed_operations: return "One operation at a time: rename, move or delete, not a mix of them.";
		case edt::refusal_code::overlapping_requests: return "Two of the files or folders asked for would end up in different places.";
		case edt::refusal_code::into_itself: return "A folder cannot go into itself or into a folder inside it.";
		case edt::refusal_code::folder_in_lower_layer: return "The folder is also in a lower layer: its files there would stay under the old name.";
		case edt::refusal_code::folder_has_foreign_entries: return "The folder holds files the project does not list, or a link: they are never deleted or followed.";
		case edt::refusal_code::too_many_files: return "Too many files or bytes for one operation.";
		}
		return "The operation was refused.";
	}

	std::string label_of(edt::asset_operation operation)
	{
		switch (operation) {
		case edt::asset_operation::rename: return "Renamed";
		case edt::asset_operation::move: return "Moved";
		case edt::asset_operation::remove: return "Deleted";
		}
		return "Changed";
	}
}

std::string edt::refusal_message(const refusal& why)
{
	std::string text = sentence_of(why.code);
	if (!why.reason.empty()) text += "\n" + shown(why.reason);
	if (why.code == refusal_code::referrer_open) {
		std::set<std::string> documents;
		for (const asset_usage& usage : why.referrers) documents.insert(usage.file);
		if (!documents.empty()) {
			text += "\nOpen with unsaved changes:";
			for (const std::string& file : documents) text += "\n  " + shown(file);
		}
	} else if (!why.referrers.empty()) {
		text += std::format("\n{} place(s) refer to it:", why.referrers.size());
		std::vector<std::string> lines;
		add_places(lines, why.referrers, k_listed_places);
		for (const std::string& line : lines) text += "\n" + line;
	}
	if (!why.unrewritable.empty()) {
		text += "\nCannot be rewritten:";
		std::vector<std::string> lines;
		add_failures(lines, why.unrewritable);
		for (const std::string& line : lines) text += "\n" + line;
	}
	if (!why.skipped.empty()) {
		text += "\nNot read:";
		for (std::size_t i = 0; i < why.skipped.size() && i < k_listed_files; ++i) text += "\n  " + shown(why.skipped[i]);
		if (why.skipped.size() > k_listed_files) text += std::format("\n  ... and {} more file(s)", why.skipped.size() - k_listed_files);
	}
	return text;
}

edt::asset_operation_form::asset_operation_form(asset_operation_ports ports)
	: m_ports(std::move(ports))
{
}

void edt::asset_operation_form::open(asset_operation what, const res::tag& asset)
{
	m_operation = what;
	m_asset = asset;
	m_error.clear();
	m_run = asset_run{};
	m_undone = false;
	m_undo_dropped = false;
	m_undo_error.clear();
	const std::string relative = relative_of(asset);
	const std::size_t slash = relative.find_last_of('/');
	if (what == asset_operation::rename) {
		m_input = slash == std::string::npos ? relative : relative.substr(slash + 1);
	} else if (what == asset_operation::move) {
		m_input = slash == std::string::npos ? std::string{} : relative.substr(0, slash);
	} else {
		m_input.clear();
	}
	m_stage = stage::editing;
	replan();
}

void edt::asset_operation_form::close()
{
	m_stage = stage::closed;
	m_item.reset();
	m_refused.reset();
	m_run = asset_run{};
}

void edt::asset_operation_form::set_input(std::string input)
{
	if (input == m_input && !m_pending) return;
	m_input = std::move(input);
	m_error.clear();
	replan();
}

void edt::asset_operation_form::type_input(std::string input, std::chrono::steady_clock::time_point now)
{
	if (input == m_input) return;
	m_input = std::move(input);
	m_error.clear();
	m_pending = true;
	m_typed_at = now;
}

void edt::asset_operation_form::settle(std::chrono::steady_clock::time_point now)
{
	if (m_pending && now - m_typed_at >= input_delay) replan();
}

bool edt::asset_operation_form::take_disk_changed()
{
	return std::exchange(m_disk_changed, false);
}

std::string edt::asset_operation_form::asset_text() const
{
	return shown(m_asset.view());
}

void edt::asset_operation_form::replan()
{
	m_pending = false;
	m_item.reset();
	m_refused.reset();
	if (!m_ports.plan) {
		m_refused = refusal{ refusal_code::no_database, "no project is open", {}, {}, {}, {} };
		return;
	}
	const asset_request asked = request();
	operation_plan plan = m_ports.plan(asked);
	if (plan.refused) {
		m_refused = std::move(plan.refused);
	} else if (plan.items.size() != 1) {
		m_refused = refusal{ refusal_code::one_asset_only, "one asset at a time", {}, {}, {}, {} };
	} else {
		m_item = std::move(plan.items.front());
	}
}

edt::asset_request edt::asset_operation_form::request() const
{
	asset_request out;
	out.operation = m_operation;
	out.asset = m_asset;
	if (m_operation != asset_operation::remove) out.to = m_input;
	return out;
}

std::string edt::asset_operation_form::refusal_text() const
{
	return m_refused ? refusal_message(*m_refused) : std::string{};
}

std::vector<std::string> edt::asset_operation_form::preview_lines() const
{
	std::vector<std::string> out;
	if (!m_item) return out;
	const plan_item& item = *m_item;
	const std::string source = shown(m_asset.view());
	if (m_operation == asset_operation::remove) {
		out.push_back("Delete " + source);
		out.push_back(std::format("{} file(s) go to the editor's journal folder; Edit > Undo Asset Action brings them back:", item.moves.size()));
		for (const planned_move& move : item.moves) out.push_back("  " + shown(res::tag::make(move.from).view()));
		if (!item.referrers.empty()) {
			out.push_back(std::format("{} place(s) refer to it:", item.referrers.size()));
			add_places(out, item.referrers, k_listed_places);
		}
		if (item.scan_problem) out.push_back(shown(item.scan_problem->reason));
		for (const std::string& warning : item.warnings) out.push_back(shown(warning));
		return out;
	}
	out.push_back(std::format("{} {}  ->  {}", m_operation == asset_operation::rename ? "Rename" : "Move", source, shown(item.target.view())));
	out.push_back(std::format("{} file(s) move together:", item.moves.size()));
	for (const planned_move& move : item.moves) {
		out.push_back(std::format("  {}  ->  {}", shown(res::tag::make(move.from).view()), shown(res::tag::make(move.to).view())));
	}
	if (!item.rewrites.empty()) {
		std::size_t places = 0;
		for (const planned_rewrite& rewrite : item.rewrites) places += rewrite.pointers.size();
		out.push_back(std::format("{} reference(s) in {} file(s) are rewritten:", places, item.rewrites.size()));
		for (std::size_t i = 0; i < item.rewrites.size() && i < k_listed_files; ++i) {
			out.push_back(std::format("  {}  ({})", shown(item.rewrites[i].file), item.rewrites[i].pointers.size()));
			add_pointers(out, item.rewrites[i].pointers);
		}
		if (item.rewrites.size() > k_listed_files) out.push_back(std::format("  ... and {} more file(s)", item.rewrites.size() - k_listed_files));
	}
	if (!item.could_not_rewrite.empty()) {
		out.push_back(std::format("{} file(s) cannot be rewritten and keep the old path:", item.could_not_rewrite.size()));
		add_failures(out, item.could_not_rewrite);
	}
	if (!item.broken_references.empty()) {
		out.push_back(std::format("{} reference(s) written as a path only stay and will not find the asset:", item.broken_references.size()));
		add_places(out, item.broken_references, k_listed_places);
	}
	for (const std::string& warning : item.warnings) out.push_back(shown(warning));
	return out;
}

bool edt::asset_operation_form::needs_force() const
{
	return m_operation == asset_operation::remove && m_item && (m_item->needs_force || m_item->scan_problem.has_value());
}

bool edt::asset_operation_form::can_submit() const
{
	return m_stage == stage::editing && !m_pending && m_item && !m_refused && !needs_force();
}

bool edt::asset_operation_form::can_ask_force() const
{
	return m_stage == stage::editing && m_item && !m_refused && needs_force();
}

void edt::asset_operation_form::ask_force()
{
	if (!can_ask_force()) return;
	m_error.clear();
	m_stage = stage::confirming_force;
}

void edt::asset_operation_form::back()
{
	if (m_stage == stage::confirming_force) m_stage = stage::editing;
}

std::string edt::asset_operation_form::force_text() const
{
	if (!m_item) return {};
	operation_plan plan;
	plan.items.push_back(*m_item);
	return operation_question(plan, true);
}

bool edt::asset_operation_form::submit()
{
	return m_stage == stage::editing && can_submit() && run(false);
}

bool edt::asset_operation_form::submit_force()
{
	return m_stage == stage::confirming_force && m_item && !m_refused && needs_force() && run(true);
}

bool edt::asset_operation_form::run(bool force)
{
	m_error.clear();
	if (!m_ports.run) {
		m_error = "No project is open.";
		return false;
	}
	m_run = m_ports.run(request(), force);
	m_disk_changed = m_disk_changed || (m_run.state != asset_run_state::blocked && m_run.state != asset_run_state::no_project);
	switch (m_run.state) {
	case asset_run_state::done:
		m_stage = stage::reported;
		m_item.reset();
		m_refused.reset();
		return true;
	case asset_run_state::blocked:
	case asset_run_state::no_project:
	case asset_run_state::denied:
	case asset_run_state::stale:
		m_error = shown(m_run.message);
		return false;
	case asset_run_state::failed:
		m_error = shown(m_run.message);
		replan();
		return false;
	case asset_run_state::refused:
		m_error = m_run.refused ? refusal_message(*m_run.refused) : std::string("The operation was refused.");
		m_stage = stage::editing;
		replan();
		return false;
	}
	return false;
}

std::string edt::asset_operation_form::report_title() const
{
	return std::format("{} {}", label_of(m_run.report.journal.operation), asset_text());
}

std::vector<std::string> edt::asset_operation_form::report_lines() const
{
	std::vector<std::string> out;
	if (m_stage != stage::reported) return out;
	const operation_report& report = m_run.report;
	out.push_back(std::format("Moved ({}):", report.moved.size()));
	for (const file_move& move : report.moved) {
		out.push_back(move.to.starts_with("cache://") ? std::format("  {}  (kept in the journal)", shown(move.from))
			: std::format("  {}  ->  {}", shown(move.from), shown(move.to)));
	}
	out.push_back(std::format("Rewritten ({} file(s)):", report.rewritten.size()));
	for (std::size_t i = 0; i < report.rewritten.size() && i < k_listed_files; ++i) {
		out.push_back(std::format("  {}  ({})", shown(report.rewritten[i].file), report.rewritten[i].json_pointers.size()));
		add_pointers(out, report.rewritten[i].json_pointers);
	}
	if (report.rewritten.size() > k_listed_files) out.push_back(std::format("  ... and {} more file(s)", report.rewritten.size() - k_listed_files));
	if (!report.could_not_rewrite.empty()) {
		out.push_back(std::format("Could not be rewritten ({} file(s)):", report.could_not_rewrite.size()));
		add_failures(out, report.could_not_rewrite);
	}
	if (!report.broken_references.empty()) {
		out.push_back(std::format("Broken references ({}): they still name the old path; Problems lists them:", report.broken_references.size()));
		add_places(out, report.broken_references, k_listed_places);
	}
	for (const std::string& warning : report.warnings) out.push_back("Warning: " + shown(warning));
	return out;
}

bool edt::asset_operation_form::undo()
{
	if (!can_undo()) return false;
	m_undo_error.clear();
	if (!m_ports.undo) {
		m_undo_error = "Nothing can be undone here.";
		return false;
	}
	const asset_fix_result result = m_ports.undo(m_run.report.operation_id);
	m_disk_changed = true;
	if (!result.ok) {
		m_undo_error = shown(result.error);
		m_undo_dropped = !result.keep;
		return false;
	}
	m_undone = true;
	return true;
}
