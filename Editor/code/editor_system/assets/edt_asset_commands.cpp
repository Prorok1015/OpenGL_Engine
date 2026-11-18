#include "edt_asset_commands.h"
#include "edt_forge_types.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_stamp.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	constexpr std::size_t k_max_text = 512;
	// Lines of one list in a question or an error: the rest is counted.
	constexpr std::size_t k_listed_places = 12;
	constexpr std::size_t k_listed_files = 8;
	// The plans a person was asked about and that have not run yet, by caller and ticket.
	constexpr std::size_t k_max_shown = 32;
	constexpr auto k_shown_lifetime = cmd::command_registry::confirmation_lifetime;

	json::value parse_schema(std::string_view text, std::string_view what)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "the schema of {} is not JSON: {}", what, ec.message());
		return parsed;
	}

	bool plain_text(std::string_view text)
	{
		return text.size() <= k_max_text && std::ranges::none_of(text, [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '\\'; });
	}

	std::string who(const cmd::origin& from)
	{
		switch (from.kind) {
		case cmd::origin_kind::agent: return "agent " + from.session;
		case cmd::origin_kind::test: return "test";
		case cmd::origin_kind::game: return "game";
		case cmd::origin_kind::ui: return "ui";
		}
		return "ui";
	}

	json::object usage_json(const edt::asset_usage& usage)
	{
		return json::object{ { "file", usage.file }, { "json_pointer", usage.json_pointer }, { "kind", edt::usage_kind_name(usage.kind) },
			{ "via", usage.by_guid ? "guid" : "path" } };
	}

	// "file  pointer  (kind)" per place, the first `limit` of them, one line each, written as a person can read foreign text.
	std::string place_lines(const std::vector<edt::asset_usage>& places, std::size_t limit)
	{
		std::string out;
		for (std::size_t i = 0; i < places.size() && i < limit; ++i) {
			out += "\n  " + edt::forge::escaped_text(edt::usage_row_text(places[i]), 256);
		}
		if (places.size() > limit) out += std::format("\n  ... and {} more", places.size() - limit);
		return out;
	}

	// The same for the agent: plain text, as the registry's callers get it.
	std::string place_list(const std::vector<edt::asset_usage>& places, std::size_t limit)
	{
		std::string out;
		for (std::size_t i = 0; i < places.size() && i < limit; ++i) {
			if (i > 0) out += ", ";
			out += std::format("{}{}", places[i].file, places[i].json_pointer);
		}
		if (places.size() > limit) out += std::format(" and {} more", places.size() - limit);
		return out;
	}

	// Whether the question about `plan` is one a person can approve: a plan that will be refused is never offered as a question, so
	// a ticket of it cannot be approved and later spent on what the files have become since.
	bool plan_runs(const edt::operation_plan& plan, bool force)
	{
		if (plan.refused || plan.items.size() != 1) return false;
		const edt::plan_item& item = plan.items.front();
		if (item.request.operation != edt::asset_operation::remove) return true;
		return force || (!item.scan_problem && !item.needs_force);
	}

	void add_part(std::string& out, std::string_view part)
	{
		out += std::format("{}:", part.size());
		out += part;
		out += ';';
	}

	void add_usages(std::string& out, const std::vector<edt::asset_usage>& usages)
	{
		add_part(out, std::to_string(usages.size()));
		for (const edt::asset_usage& usage : usages) {
			add_part(out, usage.file);
			add_part(out, usage.json_pointer);
			add_part(out, edt::usage_kind_name(usage.kind));
			add_part(out, usage.by_guid ? "guid" : "path");
		}
	}

	// Everything of the plan a person's answer is about, and what the run would write: the files and their hashes, the references
	// and the text each rewritten file becomes, what stays broken, the warnings, `force`. A run that finds another plan than the
	// one a person approved does not run.
	std::string plan_fingerprint(const edt::operation_plan& plan, bool force)
	{
		std::string text;
		add_part(text, force ? "force" : "plain");
		add_part(text, std::to_string(plan.items.size()));
		for (const edt::plan_item& item : plan.items) {
			add_part(text, edt::asset_operation_name(item.request.operation));
			add_part(text, item.request.asset.view());
			add_part(text, item.request.to);
			add_part(text, item.target.view());
			add_part(text, fs_paths::generic_utf8_of(item.root));
			add_part(text, std::to_string(item.moves.size()));
			for (const edt::planned_move& move : item.moves) {
				add_part(text, move.from);
				add_part(text, move.to);
				add_part(text, move.hash);
			}
			add_usages(text, item.referrers);
			add_part(text, std::to_string(item.rewrites.size()));
			for (const edt::planned_rewrite& rewrite : item.rewrites) {
				add_part(text, rewrite.file);
				add_part(text, fs_paths::generic_utf8_of(rewrite.root));
				add_part(text, rewrite.relative);
				add_part(text, rewrite.before_hash);
				add_part(text, rewrite.after_text);
				add_part(text, std::to_string(rewrite.pointers.size()));
				for (const std::string& pointer : rewrite.pointers) add_part(text, pointer);
			}
			add_part(text, std::to_string(item.could_not_rewrite.size()));
			for (const edt::rewrite_failure& failure : item.could_not_rewrite) {
				add_part(text, failure.file);
				add_part(text, failure.code);
				add_part(text, failure.reason);
			}
			add_usages(text, item.broken_references);
			add_part(text, std::to_string(item.warnings.size()));
			for (const std::string& warning : item.warnings) add_part(text, warning);
			add_part(text, item.scan_problem ? edt::refusal_code_name(item.scan_problem->code) : "");
			add_part(text, item.needs_force ? "needs_force" : "");
		}
		res::content_hasher hasher;
		hasher.update(std::as_bytes(std::span<const char>(text.data(), text.size())));
		return hasher.text();
	}

	struct request_args
	{
		edt::asset_request request;
		bool force = false;
		std::string error;
	};

	class asset_commands
	{
	public:
		explicit asset_commands(edt::asset_command_sources sources)
			: m_sources(std::move(sources))
		{
		}

		cmd::result run(edt::asset_operation operation, const cmd::call& call)
		{
			if (call.from.kind == cmd::origin_kind::game) {
				return cmd::result::failure(cmd::codes::denied, "a running game does not change the project's assets");
			}
			if (std::optional<std::string> why = write_blocked()) return cmd::result::failure(cmd::codes::busy, std::move(*why));
			const edt::asset_ops* operations = m_sources.operations ? m_sources.operations() : nullptr;
			if (!operations) return cmd::result::failure(cmd::codes::failed, "no project is open: there are no assets to change");
			if (std::optional<std::string> why = m_sources.asset_index_wait ? m_sources.asset_index_wait() : std::nullopt) {
				return cmd::result::failure(cmd::codes::busy, std::move(*why));
			}
			const request_args args = parse(operation, call);
			if (!args.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, args.error);
			// A delete, and what `force` leaves broken, are decided by a person for this very call: the ticket the registry hands on only
			// after the person approved it. A policy that lets the call through without asking (`allow`) does not stand in for that.
			if (call.from.kind == cmd::origin_kind::agent && operation == edt::asset_operation::remove && call.confirmation.empty()) {
				return cmd::result::failure(cmd::codes::denied,
					"asset.delete is done only after a person confirmed this very call, and a policy that allows it does not replace that; it was not asked");
			}

			const edt::operation_plan plan = operations->plan(std::span<const edt::asset_request>(&args.request, 1));
			if (plan.refused) return refused(*operations, *plan.refused);
			// A plan that is not one `plan_runs` offers is refused by execute below, whatever was asked, so it needs no question behind it.
			if (call.from.kind == cmd::origin_kind::agent && plan_runs(plan, args.force)) {
				if (std::optional<cmd::result> why = check_shown(call, plan, args.force)) return std::move(*why);
			}
			const edt::operation_report report = operations->execute(plan, edt::execute_options{ args.force });
			if (report.refused) return refused(*operations, *report.refused);
			if (!report.ok) {
				const std::string reason = operations->without_host_paths(report.error);
				std::string error = std::format("the operation failed and was taken back: {}", reason);
				std::vector<std::string> stuck_steps;
				for (const std::string& step : report.rollback_failures) stuck_steps.push_back(operations->without_host_paths(step));
				if (!stuck_steps.empty()) {
					error += std::format("; {} step(s) could not be taken back, the project is half done there: {}", stuck_steps.size(), stuck_steps.front());
				}
				cmd::result out = cmd::result::failure(cmd::codes::failed, std::move(error));
				json::array stuck;
				for (const std::string& step : stuck_steps) stuck.push_back(json::string(step));
				out.value = json::object{ { "code", "operation_failed" }, { "reason", reason }, { "rollback_failures", std::move(stuck) } };
				return out;
			}

			if (m_sources.done) m_sources.done(report);
			const std::string target = report.moved.empty() ? std::string(args.request.asset.view())
				: report.moved.front().to.starts_with("cache://") ? report.moved.front().from : std::format("{} -> {}", report.moved.front().from, report.moved.front().to);
			egLOG_INFO("agent", "asset.{} {} by {}", edt::asset_operation_name(operation), target, who(call.from));
			if (m_sources.events && call.from.kind == cmd::origin_kind::agent) {
				m_sources.events->agent_action.emit(edt::agent_action_info{ "asset." + std::string(edt::asset_operation_name(operation)), target, 0, call.from.session });
			}
			edt::operation_report shown = report;
			for (std::string& warning : shown.warnings) warning = operations->without_host_paths(std::move(warning));
			for (edt::rewrite_failure& failure : shown.could_not_rewrite) failure.reason = operations->without_host_paths(std::move(failure.reason));
			return cmd::result::success(edt::operation_report_json(shown));
		}

		std::optional<std::string> describe(edt::asset_operation operation, const cmd::call& call)
		{
			const auto refused_text = [](std::string_view why) {
				return std::optional<std::string>("This request will be refused: " + edt::forge::escaped_text(why, 1024));
			};
			if (call.from.kind == cmd::origin_kind::game) return refused_text("a running game does not change the project's assets");
			if (std::optional<std::string> why = write_blocked()) return refused_text(*why);
			const edt::asset_ops* operations = m_sources.operations ? m_sources.operations() : nullptr;
			if (!operations) return refused_text("no project is open");
			const request_args args = parse(operation, call);
			if (!args.error.empty()) return refused_text(args.error);
			const edt::operation_plan plan = operations->plan(std::span<const edt::asset_request>(&args.request, 1));
			// Only a plan that can run is remembered: the ticket of a question that says "will be refused" has nothing to spend.
			if (call.from.kind == cmd::origin_kind::agent && plan_runs(plan, args.force)) {
				remember(key_of(call.from), call.confirmation, plan_fingerprint(plan, args.force));
			}
			return edt::operation_question(plan, args.force);
		}

	private:
		struct shown_plan
		{
			std::string key;
			std::string ticket;
			std::string fingerprint;
			std::chrono::steady_clock::time_point at;
		};

		static std::string key_of(const cmd::origin& from)
		{
			return std::format("{}|{}", static_cast<int>(from.kind), from.session);
		}

		void remember(std::string key, std::string ticket, std::string fingerprint)
		{
			const auto at = std::chrono::steady_clock::now();
			std::erase_if(m_shown, [&](const shown_plan& entry) { return at - entry.at > k_shown_lifetime || (entry.key == key && entry.ticket == ticket); });
			if (m_shown.size() >= k_max_shown) m_shown.erase(m_shown.begin());
			m_shown.push_back(shown_plan{ std::move(key), std::move(ticket), std::move(fingerprint), at });
		}

		// The fingerprint of what a person was asked under this ticket (empty ticket: what was described for a call the policy let
		// through); each is spent by one run.
		std::optional<std::string> take_shown(const std::string& key, const std::string& ticket)
		{
			const auto at = std::chrono::steady_clock::now();
			std::erase_if(m_shown, [&](const shown_plan& entry) { return at - entry.at > k_shown_lifetime; });
			const auto found = std::ranges::find_if(m_shown, [&](const shown_plan& entry) { return entry.key == key && entry.ticket == ticket; });
			if (found == m_shown.end()) return std::nullopt;
			std::string fingerprint = std::move(found->fingerprint);
			m_shown.erase(found);
			return fingerprint;
		}

		// An agent's run is bound to the plan its question showed: the plan is made again now and must be that one.
		std::optional<cmd::result> check_shown(const cmd::call& call, const edt::operation_plan& plan, bool force)
		{
			const std::optional<std::string> shown = take_shown(key_of(call.from), call.confirmation);
			if (!shown) {
				return cmd::result::failure(cmd::codes::denied, "the plan was not shown to a person; ask again");
			}
			if (*shown != plan_fingerprint(plan, force)) {
				cmd::result out = cmd::result::failure(cmd::codes::stale, "the plan changed since the person was asked (the files or what refers to them), so nothing was done; ask again");
				out.value = json::object{ { "code", "plan_changed" }, { "reason", "the plan is not the one the person approved" } };
				return out;
			}
			return std::nullopt;
		}

		std::optional<std::string> write_blocked() const
		{
			return m_sources.write_blocker ? m_sources.write_blocker() : std::nullopt;
		}

		static cmd::result refused(const edt::asset_ops& operations, edt::refusal why)
		{
			why.reason = operations.without_host_paths(std::move(why.reason));
			for (edt::rewrite_failure& failure : why.unrewritable) failure.reason = operations.without_host_paths(std::move(failure.reason));
			for (std::string& file : why.skipped) file = operations.without_host_paths(std::move(file));
			std::string error = std::format("[{}] {}", edt::refusal_code_name(why.code), why.reason);
			if (!why.referrers.empty()) error += std::format(" ({})", place_list(why.referrers, k_listed_places));
			if (!why.skipped.empty()) {
				error += " (not read: ";
				for (std::size_t i = 0; i < why.skipped.size(); ++i) error += (i > 0 ? ", " : "") + why.skipped[i];
				error += ")";
			}
			if (!why.unrewritable.empty()) {
				error += " (cannot be rewritten: ";
				for (std::size_t i = 0; i < why.unrewritable.size() && i < k_listed_files; ++i) {
					error += std::format("{}{} [{}]", i > 0 ? ", " : "", why.unrewritable[i].file, why.unrewritable[i].code);
				}
				error += ")";
			}
			cmd::result out = cmd::result::failure(edt::command_code_of(why.code), std::move(error));
			out.value = edt::refusal_json(why);
			return out;
		}

		static request_args parse(edt::asset_operation operation, const cmd::call& call)
		{
			request_args out;
			out.request.operation = operation;
			const json::object& args = call.args.as_object();
			const json::value* path = args.if_contains("path");
			if (!path || !path->is_string()) {
				out.error = "`path` is the asset's res:// path";
				return out;
			}
			const std::string_view text(path->as_string().data(), path->as_string().size());
			if (text.empty() || !plain_text(text)) {
				out.error = "`path` is a res:// path of at most 512 characters, without control characters or backslashes";
				return out;
			}
			const res::tag tag = text.find("://") == std::string_view::npos ? res::tag::make(text) : res::tag{ std::string(text) };
			if (tag.protocol() != res::tag::default_protocol()) {
				out.error = std::format("only res:// assets are changed, not '{}'", tag.view());
				return out;
			}
			out.request.asset = tag;
			if (operation != edt::asset_operation::remove) {
				const json::value* to = args.if_contains("to");
				if (!to || !to->is_string()) {
					out.error = operation == edt::asset_operation::rename ? "`to` is the new file name" : "`to` is the folder below res://";
					return out;
				}
				const std::string_view target(to->as_string().data(), to->as_string().size());
				if (!plain_text(target)) {
					out.error = "`to` is at most 512 characters, without control characters or backslashes";
					return out;
				}
				out.request.to = std::string(target);
			} else if (const json::value* force = args.if_contains("force"); force && force->is_bool()) {
				out.force = force->as_bool();
			}
			return out;
		}

		edt::asset_command_sources m_sources;
		std::vector<shown_plan> m_shown;
	};
}

const char* edt::command_code_of(refusal_code code)
{
	switch (code) {
	case refusal_code::database_not_ready:
	case refusal_code::referrer_open:
		return cmd::codes::busy;
	case refusal_code::stale_plan:
		return cmd::codes::stale;
	case refusal_code::not_found:
		return cmd::codes::not_found;
	case refusal_code::has_referrers:
	case refusal_code::usage_scan_skipped:
		return cmd::codes::needs_input;
	case refusal_code::no_database:
	case refusal_code::io_error:
	case refusal_code::binary_pair_blocked:
	case refusal_code::unrewritable_references:
	case refusal_code::engine_layer:
	case refusal_code::not_top_layer:
	case refusal_code::meta_in_other_layer:
		return cmd::codes::failed;
	case refusal_code::one_asset_only:
	case refusal_code::not_res_path:
	case refusal_code::bad_name:
	case refusal_code::no_such_folder:
	case refusal_code::unportable_path:
	case refusal_code::no_change:
	case refusal_code::case_only_rename:
	case refusal_code::target_exists:
	case refusal_code::binary_name_lost:
		return cmd::codes::invalid_args;
	}
	return cmd::codes::failed;
}

json::value edt::refusal_json(const refusal& why)
{
	json::array referrers;
	for (const asset_usage& usage : why.referrers) referrers.push_back(usage_json(usage));
	json::array skipped;
	for (const std::string& file : why.skipped) skipped.push_back(json::string(file));
	json::array unrewritable;
	for (const rewrite_failure& failure : why.unrewritable) {
		unrewritable.push_back(json::object{ { "file", failure.file }, { "code", failure.code }, { "reason", failure.reason } });
	}
	return json::object{ { "code", std::string(refusal_code_name(why.code)) }, { "reason", why.reason }, { "referrers", std::move(referrers) },
		{ "skipped", std::move(skipped) }, { "unrewritable", std::move(unrewritable) } };
}

json::value edt::operation_report_json(const operation_report& report)
{
	json::array moved;
	for (const file_move& move : report.moved) moved.push_back(json::object{ { "from", move.from }, { "to", move.to } });
	json::array rewritten;
	for (const rewritten_file& file : report.rewritten) {
		json::array pointers;
		for (const std::string& pointer : file.json_pointers) pointers.push_back(json::string(pointer));
		rewritten.push_back(json::object{ { "file", file.file }, { "pointers", std::move(pointers) } });
	}
	json::array could_not;
	for (const rewrite_failure& failure : report.could_not_rewrite) {
		could_not.push_back(json::object{ { "file", failure.file }, { "code", failure.code }, { "reason", failure.reason } });
	}
	json::array warnings;
	for (const std::string& warning : report.warnings) warnings.push_back(json::string(warning));
	json::array broken;
	for (const asset_usage& usage : report.broken_references) broken.push_back(usage_json(usage));
	return json::object{ { "operation", report.operation_id }, { "action", std::string(asset_operation_name(report.journal.operation)) },
		{ "moved", std::move(moved) }, { "rewritten", std::move(rewritten) }, { "could_not_rewrite", std::move(could_not) },
		{ "warnings", std::move(warnings) }, { "broken_references", std::move(broken) } };
}

std::string edt::operation_question(const operation_plan& plan, bool force)
{
	const auto refused_text = [](std::string_view why) {
		return "This request will be refused: " + forge::escaped_text(why, 1024);
	};
	if (plan.refused) {
		std::string text = refused_text(std::format("[{}] {}", refusal_code_name(plan.refused->code), plan.refused->reason));
		if (!plan.refused->referrers.empty()) text += place_lines(plan.refused->referrers, k_listed_places);
		return text;
	}
	if (plan.items.size() != 1) return refused_text("one asset at a time");
	const plan_item& item = plan.items.front();
	const std::string shown = forge::escaped_text(item.request.asset.view(), 256);

	if (item.request.operation == asset_operation::remove) {
		if (item.scan_problem && !force) return refused_text(std::format("[{}] {}", refusal_code_name(item.scan_problem->code), item.scan_problem->reason));
		if (item.needs_force && !force) {
			return refused_text(std::format("{} place(s) refer to '{}' (a delete over them is a separate question: `force`)", item.referrers.size(), shown))
				+ place_lines(item.referrers, k_listed_places);
		}
		std::string text = std::format("Delete the asset\n  {}\n{} file(s) go to the editor's journal folder and come back with Edit > Undo Asset Action.", shown, item.moves.size());
		for (const planned_move& move : item.moves) text += "\n  " + forge::escaped_text(res::tag::make(move.from).view(), 256);
		if (force) {
			if (item.needs_force) {
				text += std::format("\n\nFORCE: {} place(s) still refer to this asset and will be broken (they are left as they are, and Problems shows them):",
					item.referrers.size());
				text += place_lines(item.referrers, k_listed_places);
			} else {
				text += "\n\nForce was asked, but nothing refers to the asset.";
			}
			if (item.scan_problem) {
				text += "\n\nWho refers to the asset is not fully known: " + forge::escaped_text(item.scan_problem->reason, 512);
			}
		}
		for (const std::string& warning : item.warnings) text += "\n" + forge::escaped_text(warning, 512);
		return text;
	}

	const bool renaming = item.request.operation == asset_operation::rename;
	std::string text = std::format("{} the asset\n  {}\n  to {}\n{} file(s) move together:", renaming ? "Rename" : "Move", shown,
		forge::escaped_text(item.target.view(), 256), item.moves.size());
	for (const planned_move& move : item.moves) text += "\n  " + forge::escaped_text(res::tag::make(move.from).view(), 256);
	if (!item.rewrites.empty()) {
		std::size_t places = 0;
		for (const planned_rewrite& rewrite : item.rewrites) places += rewrite.pointers.size();
		text += std::format("\n\n{} reference(s) in {} file(s) are rewritten:", places, item.rewrites.size());
		for (std::size_t i = 0; i < item.rewrites.size() && i < k_listed_files; ++i) {
			text += std::format("\n  {}  ({})", forge::escaped_text(item.rewrites[i].file, 256), item.rewrites[i].pointers.size());
		}
		if (item.rewrites.size() > k_listed_files) text += std::format("\n  ... and {} more file(s)", item.rewrites.size() - k_listed_files);
	}
	if (!item.could_not_rewrite.empty()) {
		text += std::format("\n\n{} file(s) cannot be rewritten and keep the old path:", item.could_not_rewrite.size());
		for (std::size_t i = 0; i < item.could_not_rewrite.size() && i < k_listed_files; ++i) {
			text += std::format("\n  {}  ({})", forge::escaped_text(item.could_not_rewrite[i].file, 256), forge::escaped_text(item.could_not_rewrite[i].reason, 256));
		}
		if (item.could_not_rewrite.size() > k_listed_files) text += std::format("\n  ... and {} more file(s)", item.could_not_rewrite.size() - k_listed_files);
	}
	if (!item.broken_references.empty()) {
		text += std::format("\n\n{} reference(s) written as a path only stay and will not find the asset:", item.broken_references.size());
		text += place_lines(item.broken_references, k_listed_places);
	}
	for (const std::string& warning : item.warnings) text += "\n" + forge::escaped_text(warning, 512);
	return text;
}

std::vector<edt::registration> edt::register_asset_commands(cmd::command_registry& commands, asset_command_sources sources)
{
	auto impl = std::make_shared<asset_commands>(std::move(sources));
	std::vector<registration> out;

	constexpr std::string_view result_schema = R"json({
		"type": "object",
		"properties": {
			"operation": { "type": "string", "description": "the id of the operation: its journal is cache://asset_ops/<id>/; a person takes it back with Edit > Undo Asset Action" },
			"action": { "type": "string", "description": "rename, move or delete" },
			"moved": {
				"type": "array",
				"description": "the files that went together (the asset, its .resource, for a desc its binary file); a delete's files are in cache://asset_ops/<id>/files/",
				"items": { "type": "object", "properties": { "from": { "type": "string" }, "to": { "type": "string" } }, "required": ["from", "to"] }
			},
			"rewritten": {
				"type": "array",
				"items": { "type": "object", "properties": { "file": { "type": "string" }, "pointers": { "type": "array", "items": { "type": "string" } } }, "required": ["file", "pointers"] }
			},
			"could_not_rewrite": {
				"type": "array",
				"items": { "type": "object", "properties": { "file": { "type": "string" }, "code": { "type": "string" }, "reason": { "type": "string" } }, "required": ["file", "code", "reason"] }
			},
			"warnings": { "type": "array", "items": { "type": "string" } },
			"broken_references": {
				"type": "array",
				"description": "places that still name the old path and will not find the asset: path-only references that could not be rewritten, or what a forced delete left",
				"items": {
					"type": "object",
					"properties": { "file": { "type": "string" }, "json_pointer": { "type": "string" }, "kind": { "type": "string" }, "via": { "type": "string" } },
					"required": ["file", "json_pointer", "kind", "via"]
				}
			}
		},
		"required": ["operation", "action", "moved", "rewritten", "could_not_rewrite", "warnings", "broken_references"]
	})json";

	const auto add = [&](std::string id, std::string summary, std::string_view args, asset_operation operation, bool destructive) {
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = parse_schema(args, desc.id + " args");
		desc.result_schema = parse_schema(result_schema, desc.id + " result");
		desc.category = cmd::agent_category::scene_edit;
		desc.risk = cmd::danger::confirm;
		desc.mutates = true;
		desc.destructive = destructive;
		desc.invoke = [impl, operation](const cmd::call& call) { return impl->run(operation, call); };
		desc.describe = [impl, operation](const cmd::call& call) { return impl->describe(operation, call); };
		// The plan a person is asked about is recorded on every path that runs it (an `allow` of the policy too), and the run
		// is held to it.
		desc.describe_before_allowed_invoke = true;
		if (registration added = commands.add(std::move(desc)); added.active()) out.push_back(std::move(added));
	};

	add("asset.rename", "Rename one asset of the project's res/ folder (a person is asked each time unless a policy file allows `asset.rename`). Its .resource (and for a desc its binary file) go with it; "
			"references that name it by guid stay, references by path are rewritten in the files that hold them. The answer is a report: what moved, "
			"which files were rewritten, what could not be, and an `operation` id. Refused with `busy` while the asset database reads the files or "
			"a file that refers to the asset is open with unsaved changes; `unrewritable_references` (`failed`, nothing was changed) lists the referrers that cannot be rewritten (a lower layer's file, a guid that is another asset's, a file not laid out as the editor writes it) in the value's `unrewritable`; the refusal's value has a machine `code`",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1,"description":"the asset, res://materials/a.mat.desc or materials/a.mat.desc"},
			"to":{"type":"string","minLength":1,"description":"the new file name, without a folder"}},
			"required":["path","to"],"additionalProperties":false})json",
		asset_operation::rename, false);
	add("asset.move", "Move one asset of the project's res/ folder into another folder of it (a person is asked each time unless a policy file allows `asset.move`); the references follow as in asset.rename",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1,"description":"the asset, res://materials/a.mat.desc or materials/a.mat.desc"},
			"to":{"type":"string","description":"the folder below res:// that exists in the project's own folder; an empty string is the root"}},
			"required":["path","to"],"additionalProperties":false})json",
		asset_operation::move, false);
	add("asset.delete", "Delete one asset of the project's res/ folder (a person is asked each time, always: a policy `allow` does not lift it, the call is then denied); its files go to the editor's journal folder and a person "
			"can put them back with Edit > Undo Asset Action. Refused with `needs_input` and the list of the places that refer to it unless `force` is true; "
			"`force` leaves those references broken, and a person confirms that apart, being shown them",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1,"description":"the asset, res://materials/a.mat.desc or materials/a.mat.desc"},
			"force":{"type":"boolean","description":"delete although other files refer to it: they stay as they are and break; a person confirms it with the list of what breaks"}},
			"required":["path"],"additionalProperties":false})json",
		asset_operation::remove, true);
	return out;
}
