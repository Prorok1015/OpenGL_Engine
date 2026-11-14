#include "edt_asset_usages.h"
#include "edt_asset_database.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_asset_walk.h>
#include <boost/json.hpp>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace json = boost::json;

namespace
{
	constexpr std::int64_t k_default_page_size = 50;
	constexpr std::int64_t k_max_page_size = 200;
	constexpr std::size_t k_max_text = 512;
	constexpr std::size_t k_max_cursor = std::size_t{ 1 } << 16;

	// As a walk pairs names: the same rule the database merges files by.
	bool same_path(std::string_view a, std::string_view b, bool case_insensitive)
	{
		return a == b || (case_insensitive && res::asset_name_key(a, true) == res::asset_name_key(b, true));
	}

	std::string hex_of(std::string_view text)
	{
		std::string out;
		out.reserve(text.size() * 2);
		for (const unsigned char c : text) out += std::format("{:02x}", c);
		return out;
	}

	std::optional<std::string> text_of_hex(std::string_view hex)
	{
		if (hex.size() % 2 != 0) return std::nullopt;
		std::string out;
		out.reserve(hex.size() / 2);
		for (std::size_t i = 0; i < hex.size(); i += 2) {
			unsigned value = 0;
			const auto [end, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
			if (ec != std::errc{} || end != hex.data() + i + 2) return std::nullopt;
			out += static_cast<char>(value);
		}
		return out;
	}

	bool plain_text(std::string_view text)
	{
		return text.size() <= k_max_text && std::ranges::none_of(text, [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '\\'; });
	}

	// The key of a row for keyset paging: an asset whose usages change between two pages moves no other row.
	std::string row_key(const edt::asset_usage& usage)
	{
		return usage.file + '\n' + usage.json_pointer;
	}

	json::value parse_schema(std::string_view text, std::string_view what)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "the schema of {} is not JSON: {}", what, ec.message());
		return parsed;
	}

	cmd::result usages(const edt::asset_usage_sources& sources, const cmd::call& call)
	{
		const json::object& args = call.args.as_object();
		const json::value* path_arg = args.if_contains("path");
		const json::value* guid_arg = args.if_contains("guid");
		if ((path_arg == nullptr) == (guid_arg == nullptr)) {
			return cmd::result::failure(cmd::codes::invalid_args, "give the asset by `path` or by `guid`, one of them");
		}
		const edt::asset_database* assets = sources.assets ? sources.assets() : nullptr;
		if (!assets) {
			return cmd::result::failure(cmd::codes::failed, "no project is open: there is no asset database to look in");
		}

		edt::usage_target target;
		if (path_arg) {
			const std::string_view text(path_arg->as_string().data(), path_arg->as_string().size());
			if (!plain_text(text) || text.empty()) {
				return cmd::result::failure(cmd::codes::invalid_args, "`path` is a res:// path of at most 512 characters, without control characters or backslashes");
			}
			const res::tag tag = text.find("://") == std::string_view::npos ? res::tag::make(text) : res::tag{ std::string(text) };
			if (tag.protocol() != res::tag::default_protocol()) {
				return cmd::result::failure(cmd::codes::invalid_args,
					std::format("only res:// assets are looked up (an engine:// file by its res:// path, #891), not '{}'", tag.view()));
			}
			target.path = tag;
		} else {
			target.id = res::asset_id_from_text(std::string_view(guid_arg->as_string().data(), guid_arg->as_string().size()));
			if (!target.id || target.id->is_nil()) {
				return cmd::result::failure(cmd::codes::invalid_args, "`guid` is a UUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) that is not nil");
			}
		}

		const std::int64_t page_size = [&args] {
			const json::value* size = args.if_contains("page_size");
			return size && size->is_int64() ? std::clamp<std::int64_t>(size->as_int64(), 1, k_max_page_size) : k_default_page_size;
		}();
		std::optional<std::string> after;
		if (const json::value* cursor = args.if_contains("cursor")) {
			const std::string_view text(cursor->as_string().data(), cursor->as_string().size());
			after = !text.empty() && text.size() <= k_max_cursor ? text_of_hex(text) : std::nullopt;
			if (!after) return cmd::result::failure(cmd::codes::invalid_args, "the cursor is not one this command returned");
		}

		const std::string manifest_file = sources.manifest_file ? sources.manifest_file() : std::string{};
		const std::vector<edt::manifest_reference> manifest = sources.manifest ? sources.manifest() : std::vector<edt::manifest_reference>{};
		const edt::usage_report report = edt::find_usages(*assets, sources.descs ? sources.descs() : nullptr, target, manifest_file, manifest);

		json::object out;
		json::object asked;
		if (report.path) asked["path"] = report.path->view();
		if (report.id) asked["guid"] = res::to_text(*report.id);
		out["target"] = std::move(asked);
		out["source"] = "disk";
		out["complete"] = report.complete;
		json::array items;
		std::size_t index = 0;
		if (after) {
			while (index < report.items.size() && row_key(report.items[index]) <= *after) ++index;
		}
		for (; index < report.items.size() && static_cast<std::int64_t>(items.size()) < page_size; ++index) {
			const edt::asset_usage& usage = report.items[index];
			items.push_back(json::object{ { "file", usage.file }, { "json_pointer", usage.json_pointer },
				{ "kind", edt::usage_kind_name(usage.kind) }, { "via", usage.by_guid ? "guid" : "path" } });
		}
		if (index < report.items.size() && !items.empty()) out["next_cursor"] = hex_of(row_key(report.items[index - 1]));
		out["items"] = std::move(items);
		json::array skipped;
		if (!after) {
			for (const std::string& file : report.skipped) {
				skipped.push_back(json::object{ { "file", file }, { "reason", "too_large" } });
			}
		}
		out["skipped"] = std::move(skipped);
		return cmd::result::success(std::move(out));
	}
}

edt::usage_report edt::find_usages(const asset_database& assets, const desc::desc_system* descs, const usage_target& target,
	std::string_view manifest_file, std::span<const manifest_reference> manifest)
{
	usage_index index;
	return index.find(assets, descs, target, manifest_file, manifest);
}

void edt::usage_index::build(const asset_database& assets)
{
	m_by_guid.clear();
	m_by_path.clear();
	m_skipped.clear();
	for (const auto& [key, record] : assets.records()) {
		if (record.references_skipped) m_skipped.push_back(std::string(record.path.view()));
		for (const raw_reference& reference : references_in(record.references)) {
			const entry found{ &record, &reference };
			if (reference.guid) m_by_guid.emplace(*reference.guid, found);
			// A guid a file holds is that file's: its path no longer counts.
			if (reference.guid && !assets.find(*reference.guid).empty()) continue;
			if (reference.path.empty()) continue;
			const res::tag written{ reference.path };
			if (written.protocol() != res::tag::default_protocol()) continue;
			m_by_path.emplace(res::asset_name_key(written.view(), assets.case_insensitive()), found);
		}
	}
	m_assets = &assets;
	m_revision = assets.revision();
	m_built = true;
	++m_builds;
}

edt::usage_report edt::usage_index::find(const asset_database& assets, const desc::desc_system* descs, const usage_target& target,
	std::string_view manifest_file, std::span<const manifest_reference> manifest)
{
	if (!m_built || m_assets != &assets || m_revision != assets.revision()) build(assets);

	usage_report out;
	out.path = target.path;
	out.id = target.id;
	if (target.path) {
		// The asset as the disk spells it: "res://Sky.desc" names sky.desc where names are case insensitive.
		if (const asset_record* record = assets.find_any_case(*target.path)) {
			out.path = record->path;
			if (record->id) out.id = record->id;
		}
	} else if (target.id) {
		if (const std::vector<const asset_record*> holders = assets.find(*target.id); !holders.empty()) out.path = holders.front()->path;
	}
	out.complete = assets.walked_once() && assets.pending_hashes() == 0;
	out.skipped = m_skipped;

	const auto add = [&out, descs](const entry& found, bool by_guid) {
		if (const std::optional<usage_kind> kind = classify_reference(*found.reference, descs)) {
			out.items.push_back(asset_usage{ std::string(found.record->path.view()), found.reference->json_pointer, *kind, by_guid });
		}
	};
	if (out.id) {
		const auto [first, last] = m_by_guid.equal_range(*out.id);
		for (auto it = first; it != last; ++it) add(it->second, true);
	}
	if (out.path) {
		const auto [first, last] = m_by_path.equal_range(res::asset_name_key(out.path->view(), assets.case_insensitive()));
		for (auto it = first; it != last; ++it) {
			// Already found by its guid: the asset asked for by an ID no file holds.
			if (out.id && it->second.reference->guid == out.id) continue;
			add(it->second, false);
		}
	}
	for (const manifest_reference& reference : manifest) {
		if (!out.path || reference.path.empty()) continue;
		const res::tag written{ reference.path };
		if (!same_path(written.view(), out.path->view(), assets.case_insensitive())) continue;
		out.items.push_back(asset_usage{ std::string(manifest_file), reference.json_pointer, usage_kind::manifest, false });
	}
	std::ranges::sort(out.items, {}, [](const asset_usage& usage) { return std::tie(usage.file, usage.json_pointer); });
	return out;
}

std::string edt::usage_row_text(const asset_usage& usage)
{
	return std::format("{}  {}  ({})", usage.file, usage.json_pointer.empty() ? std::string_view{ "/" } : std::string_view{ usage.json_pointer },
		usage_kind_name(usage.kind));
}

void edt::usage_list::set_source(report_source report, std::function<std::uint64_t()> revision)
{
	m_report = std::move(report);
	m_revision = std::move(revision);
	m_asset.reset();
	m_cached.reset();
}

const edt::usage_report* edt::usage_list::report_for(const res::tag& asset)
{
	if (!m_report) return nullptr;
	const std::uint64_t now = m_revision ? m_revision() : 0;
	if (!m_asset || !(*m_asset == asset) || now != m_seen) {
		m_asset = asset;
		m_seen = now;
		m_cached = m_report(asset);
		++m_queries;
	}
	return m_cached ? &*m_cached : nullptr;
}

std::vector<edt::registration> edt::register_asset_usage_commands(cmd::command_registry& commands, asset_usage_sources sources)
{
	std::vector<registration> out;
	cmd::command_desc desc;
	desc.id = "asset.usages";
	desc.summary = "The files that refer to an asset under res:// (by its path or its guid), as saved on the disk: file, JSON pointer, kind";
	desc.category = cmd::agent_category::read;
	desc.args_schema = parse_schema(std::format(R"json({{
		"type": "object",
		"properties": {{
			"path": {{ "type": "string", "description": "res:// path of the asset (or a path relative to res://)" }},
			"guid": {{ "type": "string", "description": "the asset's ID, as its .resource holds it" }},
			"page_size": {{ "type": "integer", "minimum": 1, "maximum": {}, "description": "items per page, default {}" }},
			"cursor": {{ "type": "string", "description": "next_cursor of the previous page" }}
		}},
		"additionalProperties": false
	}})json", k_max_page_size, k_default_page_size), "asset.usages args");
	desc.result_schema = parse_schema(R"json({
		"type": "object",
		"properties": {
			"target": { "type": "object", "properties": { "path": { "type": "string" }, "guid": { "type": "string" } } },
			"source": { "type": "string", "description": "disk: the files as saved; an unsaved edit is not seen" },
			"complete": { "type": "boolean", "description": "false while the asset database still reads files" },
			"items": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"file": { "type": "string" },
						"json_pointer": { "type": "string" },
						"kind": { "type": "string", "description": "field, parent, component, loose (no schema field: a custom field, the record of a prefab instance) or manifest" },
						"via": { "type": "string", "description": "guid or path" }
					},
					"required": ["file", "json_pointer", "kind", "via"]
				}
			},
			"skipped": {
				"type": "array",
				"description": "desc files too large to be read for references; on the first page only",
				"items": {
					"type": "object",
					"properties": { "file": { "type": "string" }, "reason": { "type": "string" } },
					"required": ["file", "reason"]
				}
			},
			"next_cursor": { "type": "string" }
		},
		"required": ["target", "source", "complete", "items", "skipped"]
	})json", "asset.usages result");
	desc.invoke = [sources = std::move(sources)](const cmd::call& call) { return usages(sources, call); };
	if (registration added = commands.add(std::move(desc)); added.active()) out.push_back(std::move(added));
	return out;
}
