#include "edt_asset_problems.h"
#include <snk/common/logger/engine_log.h>
#include <snk/common/text_escape.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_tag.h>
#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{
	// A file name in a question: one line, nothing hidden.
	constexpr std::size_t k_shown_path_bytes = 512;

	// What a reference points at that nothing in the project answers to: a guid no file holds (its path, when it has one, names no
	// file either), or a res:// path that is no file. A path with a '#' or '?' addresses a part of a file, which the database does
	// not list. Only the saved files are looked at, as in "Used by".
	std::optional<std::string> missing_target_of(const edt::asset_database& assets, const edt::raw_reference& reference)
	{
		if (reference.guid && !assets.find(*reference.guid).empty()) {
			return std::nullopt;
		}
		if (reference.path.empty()) {
			return reference.guid ? std::optional<std::string>(std::format("the asset ID {}", res::to_text(*reference.guid))) : std::nullopt;
		}
		constexpr std::string_view protocol = "res://";
		if (!reference.path.starts_with(protocol) || reference.path.find_first_of("#?") != std::string::npos) {
			return std::nullopt;
		}
		const res::tag target{ reference.path };
		if (assets.find(target) || assets.find_any_case(target)) {
			return std::nullopt;
		}
		return reference.guid ? std::format("the asset ID {} (last known as {})", res::to_text(*reference.guid), reference.path) : reference.path;
	}

	bool same(const edt::problem& a, const edt::problem& b)
	{
		return a.severity == b.severity && a.file == b.file && a.line == b.line && a.column == b.column && a.code == b.code
			&& a.message == b.message && a.notes == b.notes;
	}
}

void edt::asset_problem_publisher::publish(problem_list& problems, const asset_database& assets)
{
	if (m_revision && *m_revision == assets.revision()) {
		return;
	}
	m_revision = assets.revision();
	const std::size_t top = assets.top_layer();

	std::vector<entry> found;
	// Files of the project's own layer without a .resource, by content hash: where an orphan may go back to.
	std::multimap<std::string, std::string> free_by_hash;
	for (const auto& [key, record] : assets.records()) {
		if (!record.meta_error.empty()) {
			entry row;
			row.what = kind::bad_meta;
			row.path = key;
			row.item.severity = problem_severity::warning;
			row.item.file = key;
			row.item.code = "asset.bad_meta";
			row.item.message = std::format("the .resource cannot be read: {}", record.meta_error);
			found.push_back(std::move(row));
		}
		for (const edt::raw_reference& reference : edt::references_in(record.references)) {
			const std::optional<std::string> target = missing_target_of(assets, reference);
			if (!target) {
				continue;
			}
			entry row;
			row.what = kind::broken_reference;
			row.path = key;
			row.item.severity = reference.guid ? problem_severity::error : problem_severity::warning;
			row.item.file = key;
			row.item.code = "asset.broken_reference";
			row.item.message = std::format("{} refers to {}, which is not in the project", reference.json_pointer, *target);
			found.push_back(std::move(row));
		}
		if (record.type == "desc" && record.binary_ranges && !record.binary_beside) {
			entry row;
			row.what = kind::missing_binary;
			row.path = key;
			row.item.severity = problem_severity::error;
			row.item.file = key;
			row.item.code = "asset.missing_binary";
			row.item.message = "the desc keeps its data in a binary file that is not beside it: restore the .bin or import the "
				"model again";
			found.push_back(std::move(row));
		}
		if (record.type == "desc" && record.binary_sidecar_beside) {
			entry row;
			row.what = kind::binary_sidecar;
			row.path = binary_sidecar_path(key);
			row.link_to = key;
			row.in_top_layer = record.layer == top;
			row.item.severity = problem_severity::warning;
			row.item.file = row.path;
			row.item.code = "asset.binary_sidecar";
			row.item.message = "a .resource of a binary file whose desc is beside it: the binary file has no ID of its own";
			found.push_back(std::move(row));
		}
		const std::string_view name = std::string_view{ key }.substr(key.find_last_of('/') + 1);
		if (is_orphan_binary(record)) {
			entry row;
			row.what = kind::orphan_binary;
			row.path = key;
			row.in_top_layer = record.layer == top;
			row.item.severity = problem_severity::warning;
			row.item.file = key;
			row.item.code = "asset.orphan_binary";
			row.item.message = "a binary file without its desc: it gets no asset ID";
			found.push_back(std::move(row));
			continue;
		}
		// A binary file not hashed yet may be one: no "no ID" row before its header is known.
		if (record.layer != top || record.has_meta || (is_desc_binary_name(name) && record.content_hash.empty())) {
			continue;
		}
		entry row;
		row.what = kind::no_id;
		row.path = key;
		row.in_top_layer = true;
		row.item.severity = problem_severity::warning;
		row.item.file = key;
		row.item.code = "asset.no_id";
		row.item.message = "no .resource: the file has no asset ID";
		found.push_back(std::move(row));
		if (!record.content_hash.empty()) {
			free_by_hash.emplace(record.content_hash, key);
		}
	}

	for (const asset_orphan& orphan : assets.orphans()) {
		entry row;
		row.what = kind::orphan;
		row.path = orphan.meta_path.string();
		row.item.severity = problem_severity::warning;
		row.item.file = row.path;
		row.item.code = "asset.orphan";
		row.item.message = "a .resource without its file";
		if (orphan.layer == top && !orphan.last_hash.empty() && free_by_hash.count(orphan.last_hash) == 1) {
			row.link_to = free_by_hash.find(orphan.last_hash)->second;
		}
		found.push_back(std::move(row));
	}

	for (const res::asset_id& id : assets.duplicates()) {
		std::vector<const asset_record*> holders = assets.find(id);
		if (holders.size() < 2) {
			continue;
		}
		// The one that keeps the ID is the original; the asset index serves it too.
		std::ranges::sort(holders, [](const asset_record* a, const asset_record* b) { return holds_id_before(*a, *b); });
		const std::string& original = holders.front()->path.string();
		// Only the project's own files can be changed here: a group wholly below it is a warning without a fix.
		const bool fixable = std::ranges::any_of(holders, [top](const asset_record* holder) { return holder->layer == top; });
		for (std::size_t i = 1; i < holders.size(); ++i) {
			entry row;
			row.what = kind::duplicate_id;
			row.path = holders[i]->path.string();
			// Its .resource may come from a lower layer, which a fix must not touch.
			row.in_top_layer = holders[i]->layer == top && holders[i]->meta_layer == top;
			row.item.severity = fixable ? problem_severity::error : problem_severity::warning;
			row.item.file = row.path;
			row.item.code = "asset.duplicate_id";
			if (!fixable) {
				row.item.message = std::format("the asset ID {} is also used by {} (outside the project: not fixable "
					"here)", res::to_text(id), original);
			} else if (holders.front()->layer != top && holders[i]->layer == top) {
				// #902: an engine file keeps its ID, so references to the copy stay paths until it gets its own.
				row.item.message = std::format("the asset ID {} belongs to {} of a lower layer, which keeps it: this "
					"copy has no ID and references to it stay paths; give it a new ID", res::to_text(id), original);
			} else {
				row.item.message = std::format("the asset ID {} is also used by {}", res::to_text(id), original);
			}
			found.push_back(std::move(row));
		}
	}

	std::ranges::sort(found, [](const entry& a, const entry& b) {
		return std::tie(a.item.code, a.item.file, a.item.message) < std::tie(b.item.code, b.item.file, b.item.message);
	});
	const bool was_published = !m_last.empty();
	const bool unchanged = std::ranges::equal(found, m_last, [](const entry& a, const entry& b) {
		return same(a.item, b.item) && a.link_to == b.link_to && a.in_top_layer == b.in_top_layer;
	});
	m_last = std::move(found);
	if (unchanged) {
		return;
	}
	if (m_last.empty()) {
		if (was_published) {
			problems.clear(source);
		}
		return;
	}
	std::vector<problem> rows;
	rows.reserve(m_last.size());
	for (const entry& row : m_last) {
		rows.push_back(row.item);
	}
	problems.set(source, std::move(rows));
}

const edt::asset_problem_publisher::entry* edt::asset_problem_publisher::find(const problem& row) const
{
	const auto it = std::ranges::find_if(m_last, [&row](const entry& candidate) { return same(candidate.item, row); });
	return it == m_last.end() ? nullptr : &*it;
}

std::optional<std::string> edt::asset_problem_publisher::file_of(const problem& row) const
{
	const entry* found = find(row);
	if (!found) {
		return std::nullopt;
	}
	return found->path;
}

std::vector<edt::problem_action> edt::asset_problem_publisher::actions_of(const problem& row, asset_database& assets) const
{
	std::vector<problem_action> out;
	const entry* found = find(row);
	if (!found) {
		return out;
	}
	const res::tag path{ found->path };
	switch (found->what) {
	case kind::no_id:
		out.push_back({ "Create .resource", [&assets, path] { return assets.create_meta(path); }, {} });
		break;
	case kind::orphan:
		if (!found->link_to.empty()) {
			out.push_back({ std::format("Link to {}", found->link_to),
				[&assets, path, file = res::tag{ found->link_to }] { return assets.link_orphan(path, file); }, {} });
		}
		break;
	case kind::duplicate_id:
		if (found->in_top_layer) {
			out.push_back({ "New ID for this copy", [&assets, path] { return assets.new_id_for_copy(path); }, {} });
		}
		break;
	case kind::orphan_binary:
		if (found->in_top_layer) {
			out.push_back({ "Delete", [&assets, path] { return assets.delete_orphan_binary(path); },
				std::format("Delete {}? It is a binary file without its desc. Undo in Problems brings it back until the editor "
					"is closed.", text_escape::escaped_text(found->path, k_shown_path_bytes)) });
		}
		break;
	case kind::binary_sidecar:
		if (found->in_top_layer) {
			out.push_back({ "Remove the sidecar", [&assets, desc = res::tag{ found->link_to }] { return assets.remove_binary_sidecar(desc); },
				std::format("Remove {}? The binary file beside its desc needs no ID. Undo in Problems brings it back until the "
					"editor is closed.", text_escape::escaped_text(found->path, k_shown_path_bytes)) });
		}
		break;
	case kind::bad_meta:
	case kind::missing_binary:
	case kind::broken_reference:
		break;
	}
	return out;
}

void edt::run_person_fix(const problem_action& action, const fix_question& ask, std::function<std::optional<problem_action>()> again)
{
	const auto run = [](const std::string& label, const std::function<asset_fix_result()>& fix) {
		if (const asset_fix_result result = fix(); !result.ok) {
			egLOG_WARN("assets", "{}: {}", label, result.error);
		}
	};
	if (action.confirm.empty()) {
		run(action.label, action.run);
		return;
	}
	const bool asked = ask && ask(action, [run, label = action.label, question = action.confirm, again = std::move(again)](bool yes) {
		if (!yes) {
			return;
		}
		std::optional<problem_action> now;
		if (again) {
			now = again();
		}
		if (!now || now->confirm != question) {
			egLOG_INFO("assets", "{}: not done, the problem or the project changed since the question", label);
			return;
		}
		run(label, now->run);
	});
	if (!asked) {
		egLOG_WARN("assets", "{}: refused, nobody can be asked", action.label);
	}
}
