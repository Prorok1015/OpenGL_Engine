#include "edt_save_all.h"
#include "edt_data_document.h"
#include "edt_prefab_document.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_document_manager.h>
#include <algorithm>
#include <format>
#include <utility>

namespace json = boost::json;

namespace {
	std::string_view kind_heading(std::string_view kind)
	{
		if (kind == "scene") return "Level";
		if (kind == "prefab") return "Prefab";
		if (kind == "data") return "Data";
		return kind;
	}

	std::optional<edt::save_all_question> question_of(const edt::document& open)
	{
		if (const auto* data = dynamic_cast<const edt::data_document*>(&open)) {
			if (data->changed_on_disk()) return edt::save_all_question::disk_changed;
			if (data->needs_version_confirmation()) return edt::save_all_question::version;
			return std::nullopt;
		}
		if (const auto* prefab = dynamic_cast<const edt::prefab_document*>(&open)) {
			if (prefab->changed_on_disk()) return edt::save_all_question::disk_changed;
			if (prefab->layer_refused()) return edt::save_all_question::base_layer;
		}
		return std::nullopt;
	}

	void write_scene(const edt::document& open, edt::unsaved_item item, const edt::save_all_scene_ports& scene, edt::save_all_report& report)
	{
		if (open.edits_locked()) {
			report.skipped.push_back({ std::move(item), "Play runs: stop it, then save the level" });
			return;
		}
		if (const std::optional<std::string> why = scene.write_blocker ? scene.write_blocker() : std::nullopt) {
			report.failed.push_back({ std::move(item), *why });
			return;
		}
		if (!scene.level_path || scene.level_path().empty()) {
			report.skipped.push_back({ std::move(item), "untitled: use Save Level As" });
			return;
		}
		if (!scene.quick_save || !scene.quick_save()) {
			report.failed.push_back({ std::move(item), "the level could not be written" });
			return;
		}
		report.saved.push_back(std::move(item));
	}

	json::object item_json(const edt::unsaved_item& item)
	{
		return json::object{ { "id", static_cast<std::uint64_t>(item.id) }, { "kind", item.kind }, { "title", item.title } };
	}

	template <typename Items, typename Fill>
	json::array array_of(const Items& items, Fill fill)
	{
		json::array out;
		out.reserve(items.size());
		for (const auto& entry : items) {
			out.push_back(fill(entry));
		}
		return out;
	}

	void list_lines(std::string& text, std::string_view heading, const std::vector<std::string>& lines)
	{
		if (lines.empty()) return;
		text += std::format("\n{}:", heading);
		for (const std::string& line : lines) text += "\n  " + line;
		text += "\n";
	}

	std::string line_of(const edt::unsaved_item& item, std::string_view why)
	{
		return std::format("'{}' ({}): {}", item.title, kind_heading(item.kind), why);
	}

	struct resolve_state
	{
		edt::save_all_report report;
		std::vector<edt::save_all_ask> queue;
		std::size_t next = 0;
		edt::save_all_asker ask;
		std::function<void(edt::save_all_report)> finished;
	};

	void ask_next(const std::shared_ptr<resolve_state>& state)
	{
		if (state->next >= state->queue.size()) {
			state->finished(std::move(state->report));
			return;
		}
		const edt::save_all_ask current = state->queue[state->next++];
		state->ask(current, [state, current](bool resolved) {
			if (resolved) {
				state->report.settled.push_back(current.item);
			} else {
				state->report.asked.push_back(current);
			}
			ask_next(state);
		});
	}
}

std::vector<edt::unsaved_item> edt::list_unsaved(document_manager& documents)
{
	std::vector<unsaved_item> out;
	documents.for_each([&out](document& open) {
		if (open.is_dirty()) out.push_back({ open.id(), std::string(open.kind()), open.title() });
	});
	return out;
}

std::size_t edt::count_unsaved(document_manager& documents)
{
	std::size_t count = 0;
	documents.for_each([&count](document& open) {
		if (open.is_dirty()) ++count;
	});
	return count;
}

std::string edt::unsaved_text(const std::vector<unsaved_item>& items)
{
	std::string text = "Unsaved documents:";
	std::vector<std::string_view> kinds;
	for (const unsaved_item& item : items) {
		if (std::ranges::find(kinds, std::string_view{ item.kind }) == kinds.end()) kinds.push_back(item.kind);
	}
	for (const std::string_view kind : kinds) {
		text += std::format("\n{}", kind_heading(kind));
		for (const unsaved_item& item : items) {
			if (item.kind == kind) text += "\n  " + item.title;
		}
	}
	return text;
}

edt::save_all_report edt::save_all_write(document_manager& documents, const save_all_scene_ports& scene)
{
	save_all_report report;
	const document_id scene_id = documents.scene().id();
	for (unsaved_item& item : list_unsaved(documents)) {
		document* open = documents.find(item.id);
		if (!open) continue;
		if (item.id == scene_id) {
			write_scene(*open, std::move(item), scene, report);
			continue;
		}
		const save_result saved = open->save();
		if (saved.saved) {
			report.saved.push_back(std::move(item));
			continue;
		}
		if (const std::optional<save_all_question> question = question_of(*open)) {
			report.asked.push_back({ std::move(item), *question });
			continue;
		}
		egLOG_WARN("editor", "{}", saved.error);
		report.failed.push_back({ std::move(item), saved.error });
	}
	return report;
}

void edt::save_all_resolve(save_all_report report, save_all_asker ask, std::function<void(save_all_report)> finished)
{
	if (!ask || report.asked.empty()) {
		if (finished) finished(std::move(report));
		return;
	}
	auto state = std::make_shared<resolve_state>();
	state->queue = std::move(report.asked);
	report.asked.clear();
	state->report = std::move(report);
	state->ask = std::move(ask);
	state->finished = finished ? std::move(finished) : [](save_all_report) {};
	ask_next(state);
}

std::string_view edt::save_all_question_text(save_all_question question)
{
	switch (question) {
	case save_all_question::disk_changed: return "changed on disk";
	case save_all_question::version: return "needs a yes to be written in the current version";
	case save_all_question::base_layer: return "a variant whose edits need its base dropped";
	}
	return {};
}

json::value edt::save_all_json(const save_all_report& report)
{
	json::object out;
	out["saved"] = array_of(report.saved, [](const unsaved_item& item) { return json::value(item_json(item)); });
	out["failed"] = array_of(report.failed, [](const save_all_note& note) {
		json::object entry = item_json(note.item);
		entry["error"] = note.text;
		return json::value(std::move(entry));
	});
	out["needs_input"] = array_of(report.asked, [](const save_all_ask& ask) {
		json::object entry = item_json(ask.item);
		entry["question"] = std::string(save_all_question_text(ask.question));
		return json::value(std::move(entry));
	});
	out["skipped"] = array_of(report.skipped, [](const save_all_note& note) {
		json::object entry = item_json(note.item);
		entry["reason"] = note.text;
		return json::value(std::move(entry));
	});
	return out;
}

std::string edt::save_all_summary(const save_all_report& report)
{
	if (report.empty()) return {};
	const std::size_t total = report.saved.size() + report.failed.size() + report.asked.size() + report.skipped.size() + report.settled.size();
	std::string text = std::format("Save All: saved {} of {} documents, failed {}.", report.saved.size(), total, report.failed.size());

	std::vector<std::string> lines;
	for (const save_all_note& note : report.failed) lines.push_back(line_of(note.item, note.text));
	list_lines(text, "Not saved", lines);

	lines.clear();
	for (const save_all_ask& ask : report.asked) lines.push_back(line_of(ask.item, save_all_question_text(ask.question)));
	list_lines(text, "Still unsaved (nothing was changed)", lines);

	lines.clear();
	for (const save_all_note& note : report.skipped) lines.push_back(line_of(note.item, note.text));
	list_lines(text, "Left out", lines);

	lines.clear();
	for (const unsaved_item& item : report.settled) lines.push_back(std::format("'{}' ({})", item.title, kind_heading(item.kind)));
	list_lines(text, "Settled by your answer (reloaded or overwritten)", lines);
	return text;
}
