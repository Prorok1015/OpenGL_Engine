#include "edt_asset_usages.h"
#include "edt_asset_database.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
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

	bool same_path(std::string_view a, std::string_view b, bool case_insensitive)
	{
		if (!case_insensitive) return a == b;
		return a.size() == b.size() && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
			return std::tolower(x) == std::tolower(y);
		});
	}

	// Whether a reference written as `reference` leads to the asset (`path`, `id`).
	std::optional<bool> matches(const edt::asset_database& assets, const edt::raw_reference& reference, const std::optional<res::tag>& path,
		const std::optional<res::asset_id>& id)
	{
		if (reference.guid) {
			if (id && *reference.guid == *id) return true;
			// A guid a file holds is that file's: its path no longer counts.
			if (!assets.find(*reference.guid).empty()) return std::nullopt;
		}
		if (!path || reference.path.empty()) return std::nullopt;
		const res::tag written{ reference.path };
		if (written.protocol() != res::tag::default_protocol()) return std::nullopt;
		if (!same_path(written.view(), path->view(), assets.case_insensitive())) return std::nullopt;
		return false;
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
			after = text.size() <= k_max_cursor ? text_of_hex(text) : std::nullopt;
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
		for (const std::string& file : report.skipped) {
			skipped.push_back(json::object{ { "file", file }, { "reason", "too_large" } });
		}
		out["skipped"] = std::move(skipped);
		return cmd::result::success(std::move(out));
	}
}

edt::usage_report edt::find_usages(const asset_database& assets, const desc::desc_system* descs, const usage_target& target,
	std::string_view manifest_file, std::span<const manifest_reference> manifest)
{
	usage_report out;
	out.path = target.path;
	out.id = target.id;
	if (target.path) {
		if (const asset_record* record = assets.find(*target.path); record && record->id) out.id = record->id;
	} else if (target.id) {
		if (const std::vector<const asset_record*> holders = assets.find(*target.id); !holders.empty()) out.path = holders.front()->path;
	}
	out.complete = assets.walked_once() && assets.pending_hashes() == 0;

	for (const auto& [key, record] : assets.records()) {
		if (record.references_skipped) out.skipped.push_back(std::string(record.path.view()));
		for (const raw_reference& reference : record.references) {
			const std::optional<bool> by_guid = matches(assets, reference, out.path, out.id);
			if (!by_guid) continue;
			const std::optional<usage_kind> kind = classify_reference(reference, descs);
			if (!kind) continue;
			out.items.push_back(asset_usage{ std::string(record.path.view()), reference.json_pointer, *kind, *by_guid });
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

std::vector<edt::registration> edt::register_asset_usage_commands(cmd::command_registry& commands, asset_usage_sources sources)
{
	std::vector<registration> out;
	cmd::command_desc desc;
	desc.id = "asset.usages";
	desc.summary = "The files that refer to an asset under res:// (by its path or its guid), as saved on the disk: file, JSON pointer, kind";
	desc.category = cmd::agent_category::read;
	desc.args_schema = parse_schema(std::format(R"({{
		"type": "object",
		"properties": {{
			"path": {{ "type": "string", "description": "res:// path of the asset (or a path relative to res://)" }},
			"guid": {{ "type": "string", "description": "the asset's ID, as its .resource holds it" }},
			"page_size": {{ "type": "integer", "minimum": 1, "maximum": {}, "description": "items per page, default {}" }},
			"cursor": {{ "type": "string", "description": "next_cursor of the previous page" }}
		}},
		"additionalProperties": false
	}})", k_max_page_size, k_default_page_size), "asset.usages args");
	desc.result_schema = parse_schema(R"({
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
				"items": {
					"type": "object",
					"properties": { "file": { "type": "string" }, "reason": { "type": "string" } },
					"required": ["file", "reason"]
				}
			},
			"next_cursor": { "type": "string" }
		},
		"required": ["target", "source", "complete", "items", "skipped"]
	})", "asset.usages result");
	desc.invoke = [sources = std::move(sources)](const cmd::call& call) { return usages(sources, call); };
	if (registration added = commands.add(std::move(desc)); added.active()) out.push_back(std::move(added));
	return out;
}
