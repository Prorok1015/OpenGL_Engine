#include "edt_asset_ops.h"
#include "edt_asset_database.h"
#include "edt_reference_rewrite.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_desc_binary_codec.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_stamp.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	// The biggest file an operation moves: a desc's binary file may be this big (res::desc_binary::max_file_bytes).
	constexpr std::uint64_t k_max_moved_bytes = std::uint64_t{ 512 } << 20;
	constexpr std::string_view k_journal_file = "journal.json";

	std::string_view view_of(json::string_view text)
	{
		return std::string_view(text.data(), text.size());
	}

	std::string utf8(const fs::path& path)
	{
		return fs_paths::generic_utf8_of(path);
	}

	fs::path path_of(const std::string& relative)
	{
		return fs_paths::path_from_utf8(relative);
	}

	std::string parent_of(std::string_view relative)
	{
		const std::size_t slash = relative.find_last_of('/');
		return slash == std::string_view::npos ? std::string{} : std::string(relative.substr(0, slash));
	}

	std::string join(std::string_view folder, std::string_view name)
	{
		return folder.empty() ? std::string(name) : std::format("{}/{}", folder, name);
	}

	std::string hash_of(std::span<const std::byte> bytes)
	{
		res::content_hasher hasher;
		hasher.update(bytes);
		return hasher.text();
	}

	std::string hash_of(const std::string& text)
	{
		return hash_of(std::as_bytes(std::span<const char>(text.data(), text.size())));
	}

	platform::beneath::read_request read_request_of(const fs::path& root, const std::string& relative, std::uint64_t max_bytes)
	{
		const fs::path path = path_of(relative);
		platform::beneath::read_request request;
		request.root = root;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.max_bytes = max_bytes;
		return request;
	}

	std::string read_error(const platform::beneath::read_result& result)
	{
		if (result.status == platform::beneath::read_status::ok) return {};
		return result.error.empty() ? std::string{ "it cannot be read" } : result.error;
	}

	// Empty `error`: the hash of the file's bytes, read in chunks.
	std::string hash_file(const fs::path& root, const std::string& relative, std::string& error)
	{
		res::content_hasher hasher;
		const auto result = platform::beneath::read_beneath_stream(read_request_of(root, relative, (std::numeric_limits<std::uint64_t>::max)()),
			[&hasher](std::span<const std::byte> chunk) {
				hasher.update(chunk);
				return true;
			});
		error = read_error(result);
		return error.empty() ? hasher.text() : std::string{};
	}

	std::string read_file(const fs::path& root, const std::string& relative, std::uint64_t max_bytes, std::vector<std::byte>& bytes)
	{
		bytes.clear();
		return read_error(platform::beneath::read_beneath(read_request_of(root, relative, max_bytes), bytes));
	}

	fs::path temp_leaf_for(const fs::path& leaf)
	{
		std::random_device random;
		fs::path out{ "." };
		out += leaf;
		out += std::format(".{:08x}.tmp", random());
		return out;
	}

	std::string write_file(const fs::path& root, const std::string& relative, std::span<const std::byte> bytes, bool replace)
	{
		const fs::path path = path_of(relative);
		platform::beneath::pinned_root pinned{ root };
		platform::beneath::write_request request;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.tmp_leaf = temp_leaf_for(request.leaf);
		request.data = bytes;
		request.replace = replace;
		request.tmp_path = root / request.folder / request.tmp_leaf;
		return platform::beneath::write_beneath(pinned, request);
	}

	std::string move_file(const fs::path& root, const std::string& from, const std::string& to)
	{
		const fs::path source = path_of(from);
		const fs::path target = path_of(to);
		platform::beneath::pinned_root pinned{ root };
		platform::beneath::rename_request request;
		request.from_folder = source.parent_path();
		request.from_leaf = source.filename();
		request.to_folder = target.parent_path();
		request.to_leaf = target.filename();
		return platform::beneath::rename_beneath(pinned, request);
	}

	std::string remove_file(const fs::path& root, const std::string& relative)
	{
		const fs::path path = path_of(relative);
		platform::beneath::pinned_root pinned{ root };
		return platform::beneath::remove_beneath(pinned, path.parent_path(), path.filename(), true);
	}

	bool exists_on_disk(const fs::path& root, const std::string& relative)
	{
		std::error_code ec;
		return fs::symlink_status(root / path_of(relative), ec).type() != fs::file_type::not_found;
	}

	bool is_regular(const fs::path& root, const std::string& relative)
	{
		std::error_code ec;
		return fs::is_regular_file(fs::symlink_status(root / path_of(relative), ec));
	}

	// The journal folder is the editor's own (cache://asset_ops/), so plain file operations do.
	std::string write_plain(const fs::path& path, std::span<const std::byte> bytes)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		if (ec) return ec.message();
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		out.flush();
		return out ? std::string{} : std::format("cannot write {}", utf8(path));
	}

	std::string read_plain(const fs::path& path, std::vector<std::byte>& bytes)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return std::format("cannot read {}", utf8(path));
		bytes.clear();
		std::vector<char> chunk(std::size_t{ 1 } << 16);
		while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
			const auto* first = reinterpret_cast<const std::byte*>(chunk.data());
			bytes.insert(bytes.end(), first, first + in.gcount());
		}
		return {};
	}

	std::string hash_plain(const fs::path& path, std::string& error)
	{
		std::vector<std::byte> bytes;
		error = read_plain(path, bytes);
		return error.empty() ? hash_of(bytes) : std::string{};
	}

	edt::refusal stop(edt::refusal_code code, std::string reason)
	{
		edt::refusal out;
		out.code = code;
		out.reason = std::move(reason);
		return out;
	}

	std::string_view step_kind_name(edt::journal_step_kind kind)
	{
		switch (kind) {
		case edt::journal_step_kind::move: return "move";
		case edt::journal_step_kind::rewrite: return "rewrite";
		case edt::journal_step_kind::stash: return "stash";
		}
		return "move";
	}

	std::optional<edt::journal_step_kind> step_kind_of(std::string_view name)
	{
		for (const edt::journal_step_kind kind : { edt::journal_step_kind::move, edt::journal_step_kind::rewrite, edt::journal_step_kind::stash }) {
			if (step_kind_name(kind) == name) return kind;
		}
		return std::nullopt;
	}

	std::string step_label(const edt::journal_step& step)
	{
		return std::format("{} {}", step_kind_name(step.kind), step.from);
	}

	// Takes one step back. Empty: done.
	std::string undo_step(const edt::journal_step& step, const fs::path& directory)
	{
		const fs::path root = path_of(step.root);
		std::vector<std::byte> bytes;
		switch (step.kind) {
		case edt::journal_step_kind::move:
			return move_file(root, step.to, step.from);
		case edt::journal_step_kind::rewrite:
			if (std::string error = read_plain(directory / path_of(step.original), bytes); !error.empty()) return error;
			return write_file(root, step.from, bytes, true);
		case edt::journal_step_kind::stash:
			if (std::string error = read_plain(directory / path_of(step.to), bytes); !error.empty()) return error;
			return write_file(root, step.from, bytes, false);
		}
		return {};
	}

	void persist(const edt::operation_journal& journal)
	{
		const std::string text = json::serialize(edt::journal_to_json(journal));
		const fs::path path = path_of(journal.directory) / std::string(k_journal_file);
		if (const std::string error = write_plain(path, std::as_bytes(std::span<const char>(text.data(), text.size()))); !error.empty()) {
			egLOG_WARN("asset_ops", "the journal of '{}' was not written: {}", journal.id, error);
		}
	}
}

std::string_view edt::asset_operation_name(asset_operation operation)
{
	switch (operation) {
	case asset_operation::rename: return "rename";
	case asset_operation::move: return "move";
	case asset_operation::remove: return "delete";
	}
	return "rename";
}

std::string_view edt::refusal_code_name(refusal_code code)
{
	switch (code) {
	case refusal_code::one_asset_only: return "one_asset_only";
	case refusal_code::no_database: return "no_database";
	case refusal_code::database_not_ready: return "database_not_ready";
	case refusal_code::not_res_path: return "not_res_path";
	case refusal_code::not_found: return "not_found";
	case refusal_code::engine_layer: return "engine_layer";
	case refusal_code::not_top_layer: return "not_top_layer";
	case refusal_code::meta_in_other_layer: return "meta_in_other_layer";
	case refusal_code::bad_name: return "bad_name";
	case refusal_code::no_such_folder: return "no_such_folder";
	case refusal_code::unportable_path: return "unportable_path";
	case refusal_code::no_change: return "no_change";
	case refusal_code::case_only_rename: return "case_only_rename";
	case refusal_code::target_exists: return "target_exists";
	case refusal_code::binary_name_lost: return "binary_name_lost";
	case refusal_code::usage_scan_skipped: return "usage_scan_skipped";
	case refusal_code::referrer_open: return "referrer_open";
	case refusal_code::has_referrers: return "has_referrers";
	case refusal_code::stale_plan: return "stale_plan";
	case refusal_code::io_error: return "io_error";
	}
	return "io_error";
}

json::value edt::journal_to_json(const operation_journal& journal)
{
	json::array steps;
	for (const journal_step& step : journal.steps) {
		steps.push_back(json::object{ { "kind", std::string(step_kind_name(step.kind)) }, { "root", step.root }, { "from", step.from }, { "to", step.to },
			{ "hash", step.hash }, { "after_hash", step.after_hash }, { "original", step.original } });
	}
	return json::object{ { "id", journal.id }, { "operation", std::string(asset_operation_name(journal.operation)) },
		{ "directory", journal.directory }, { "steps", std::move(steps) } };
}

std::optional<edt::operation_journal> edt::journal_from_json(const json::value& value)
{
	const json::object* object = value.if_object();
	if (!object) return std::nullopt;
	const auto text = [](const json::object& from, std::string_view key) -> std::optional<std::string> {
		const json::value* found = from.if_contains(json::string_view(key.data(), key.size()));
		if (!found || !found->is_string()) return std::nullopt;
		return std::string(view_of(found->as_string()));
	};
	const std::optional<std::string> id = text(*object, "id");
	const std::optional<std::string> operation = text(*object, "operation");
	const std::optional<std::string> directory = text(*object, "directory");
	const json::value* steps = object->if_contains("steps");
	if (!id || !operation || !directory || !steps || !steps->is_array()) return std::nullopt;

	operation_journal out;
	out.id = *id;
	out.directory = *directory;
	const auto named = { asset_operation::rename, asset_operation::move, asset_operation::remove };
	const auto kind = std::ranges::find_if(named, [&](asset_operation candidate) { return asset_operation_name(candidate) == *operation; });
	if (kind == named.end()) return std::nullopt;
	out.operation = *kind;
	for (const json::value& item : steps->as_array()) {
		const json::object* step = item.if_object();
		if (!step) return std::nullopt;
		const std::optional<std::string> step_kind = text(*step, "kind");
		const std::optional<std::string> root = text(*step, "root");
		const std::optional<std::string> from = text(*step, "from");
		const std::optional<std::string> to = text(*step, "to");
		const std::optional<std::string> hash = text(*step, "hash");
		const std::optional<std::string> after_hash = text(*step, "after_hash");
		const std::optional<std::string> original = text(*step, "original");
		if (!step_kind || !root || !from || !to || !hash || !after_hash || !original) return std::nullopt;
		const std::optional<journal_step_kind> parsed = step_kind_of(*step_kind);
		if (!parsed) return std::nullopt;
		out.steps.push_back(journal_step{ *parsed, *root, *from, *to, *hash, *after_hash, *original });
	}
	return out;
}

bool edt::is_engine_layer(const std::vector<fs::path>& layers, std::size_t layer, const fs::path& engine_base)
{
	if (engine_base.empty() || layer >= layers.size()) return false;
	std::error_code ec;
	const fs::path a = fs::weakly_canonical(layers[layer], ec);
	const fs::path b = ec ? fs::path{} : fs::weakly_canonical(engine_base, ec);
	if (ec) return layers[layer].lexically_normal() == engine_base.lexically_normal();
	return a == b;
}

edt::asset_ops::asset_ops(asset_ops_ports ports)
	: m_ports(std::move(ports))
{
}

edt::operation_plan edt::asset_ops::plan(std::span<const asset_request> requests) const
{
	operation_plan out;
	if (requests.size() != 1) {
		out.refused = stop(refusal_code::one_asset_only, "one asset at a time: folders and several files are not done yet");
		return out;
	}
	plan_item item;
	item.request = requests.front();
	out.refused = plan_one(item);
	if (!out.refused) out.items.push_back(std::move(item));
	return out;
}

std::optional<edt::refusal> edt::asset_ops::plan_one(plan_item& item) const
{
	const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
	if (!db) return stop(refusal_code::no_database, "no project is open: there is no asset database");
	const asset_request& request = item.request;
	const bool removing = request.operation == asset_operation::remove;
	if (request.asset.protocol() != res::tag::default_protocol()) {
		return stop(refusal_code::not_res_path, std::format("only res:// assets are changed, not '{}'", request.asset.view()));
	}
	const asset_record* record = db->find_any_case(request.asset);
	if (!record) return stop(refusal_code::not_found, std::format("'{}' is not a file of the project", request.asset.view()));
	const std::vector<fs::path>& layers = db->layers();
	if (is_engine_layer(layers, record->layer, m_ports.engine_base)) {
		return stop(refusal_code::engine_layer, std::format("'{}' belongs to the engine and is never changed", record->path.view()));
	}
	if (record->layer != db->top_layer()) {
		return stop(refusal_code::not_top_layer, std::format("'{}' is not in the project's own folder", record->path.view()));
	}
	if (record->has_meta && record->meta_layer != record->layer) {
		return stop(refusal_code::meta_in_other_layer, std::format("the .resource of '{}' is in another layer than the file", record->path.view()));
	}
	item.root = layers[record->layer];
	const std::string source = std::string(record->path.relative());

	std::string dest;
	if (!removing) {
		if (request.operation == asset_operation::rename) {
			if (request.to.empty() || request.to.find_first_of("/\\") != std::string::npos) {
				return stop(refusal_code::bad_name, "a new name is one file name, without a folder");
			}
			dest = join(parent_of(source), request.to);
		} else {
			std::string_view folder = request.to;
			if (folder.starts_with("res://")) folder.remove_prefix(6);
			while (folder.starts_with('/')) folder.remove_prefix(1);
			while (folder.ends_with('/')) folder.remove_suffix(1);
			if (folder.find('\\') != std::string_view::npos) return stop(refusal_code::bad_name, "a folder is written with '/'");
			dest = join(folder, record->path.name());
			if (!folder.empty() && !fs::is_directory(item.root / path_of(std::string(folder)))) {
				return stop(refusal_code::no_such_folder, std::format("'res://{}' is not a folder of the project's own folder", folder));
			}
		}
		if (const std::string problem = res::portable_path_problem(dest); !problem.empty()) {
			return stop(refusal_code::unportable_path, std::format("'{}': {}", dest, problem));
		}
		if (dest == source) return stop(refusal_code::no_change, "the file is there already under that name");
		if (db->case_insensitive() && res::asset_name_key(dest, true) == res::asset_name_key(source, true)) {
			return stop(refusal_code::case_only_rename, "names are the same file in any letter case here, so a change of case alone is not done");
		}
		item.target = res::tag::make(dest);
		if (db->find_any_case(item.target) || exists_on_disk(item.root, dest)) {
			return stop(refusal_code::target_exists, std::format("'{}' is taken", item.target.view()));
		}
	}

	// The files that go together: the file, its .resource, and for a desc its binary file with that one's .resource.
	const std::string meta_suffix{ res::asset_meta_suffix };
	struct pair_file { std::string from; std::string to; };
	std::vector<pair_file> pairs{ { source, dest } };
	if (is_regular(item.root, source + meta_suffix)) pairs.push_back({ source + meta_suffix, removing ? std::string{} : dest + meta_suffix });
	const std::optional<res::tag> binary = res::desc_binary::binary_tag(record->path);
	const std::optional<res::tag> dest_binary = removing ? std::nullopt : res::desc_binary::binary_tag(item.target);
	if (binary && is_regular(item.root, std::string(binary->relative()))) {
		if (!removing && !dest_binary) {
			return stop(refusal_code::binary_name_lost, std::format("the binary file of '{}' goes with a desc named <name>.<kind>.desc, which '{}' is not",
				record->path.view(), item.target.view()));
		}
		const std::string binary_source = std::string(binary->relative());
		pairs.push_back({ binary_source, removing ? std::string{} : std::string(dest_binary->relative()) });
		if (is_regular(item.root, binary_source + meta_suffix)) {
			pairs.push_back({ binary_source + meta_suffix, removing ? std::string{} : std::string(dest_binary->relative()) + meta_suffix });
		}
	}
	if (dest_binary) {
		// A binary file left at the new name would be taken by the new desc.
		for (const std::string& stray : { std::string(dest_binary->relative()), std::string(dest_binary->relative()) + meta_suffix }) {
			if (exists_on_disk(item.root, stray)) return stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(stray).view()));
		}
	}
	for (pair_file& pair : pairs) {
		if (!removing && exists_on_disk(item.root, pair.to)) {
			return stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(pair.to).view()));
		}
		std::string error;
		std::string hash = hash_file(item.root, pair.from, error);
		if (!error.empty()) return stop(refusal_code::io_error, std::format("'{}': {}", res::tag::make(pair.from).view(), error));
		item.moves.push_back(planned_move{ pair.from, removing ? "files/" + pair.from : pair.to, std::move(hash) });
	}
	if (removing) {
		const bool covers_engine = std::ranges::any_of(record->shadowed_layers, [&](std::size_t layer) { return is_engine_layer(layers, layer, m_ports.engine_base); });
		if (covers_engine) {
			item.warnings.push_back(std::format("'{}' covered a file of the engine: the engine's file shows again", record->path.view()));
		}
	}

	const std::optional<asset_ops_manifest> manifest = m_ports.manifest ? m_ports.manifest() : std::nullopt;
	const std::span<const manifest_reference> manifest_references = manifest ? std::span<const manifest_reference>(manifest->references) : std::span<const manifest_reference>{};
	usage_report usages = find_usages(*db, m_ports.descs ? m_ports.descs() : nullptr, usage_target{ record->path, record->id },
		manifest ? std::string_view(manifest->file) : std::string_view{}, manifest_references);
	std::erase_if(usages.items, [&](const asset_usage& usage) { return usage.file == record->path.view(); });
	item.referrers = usages.items;

	std::optional<refusal> scan_problem;
	if (!usages.skipped.empty()) {
		scan_problem = stop(refusal_code::usage_scan_skipped, "some desc files are too big to be read for references, so who refers to the asset is not known");
		scan_problem->skipped = usages.skipped;
	} else if (!usages.complete) {
		scan_problem = stop(refusal_code::database_not_ready, "the asset database is still reading the files");
	}
	if (removing) {
		item.scan_problem = std::move(scan_problem);
		item.needs_force = !item.referrers.empty();
		return std::nullopt;
	}
	if (scan_problem) return scan_problem;

	std::map<std::string, std::vector<const asset_usage*>> groups;
	for (const asset_usage& usage : item.referrers) groups[usage.file].push_back(&usage);
	if (m_ports.is_open_with_unsaved_changes) {
		if (m_ports.is_open_with_unsaved_changes(record->path)) {
			return stop(refusal_code::referrer_open, std::format("'{}' is open with unsaved changes", record->path.view()));
		}
		for (const auto& entry : groups) {
			const std::string& file = entry.first;
			const std::vector<const asset_usage*>& group = entry.second;
			if (group.front()->kind == usage_kind::manifest || !m_ports.is_open_with_unsaved_changes(res::tag{ file })) continue;
			refusal open = stop(refusal_code::referrer_open, std::format("'{}' refers to the asset and is open with unsaved changes", file));
			for (const asset_usage* usage : group) open.referrers.push_back(*usage);
			return open;
		}
	}

	for (const auto& entry : groups) {
		const std::string& file = entry.first;
		const std::vector<const asset_usage*>& group = entry.second;
		const auto reject = [&](std::string code, std::string reason) {
			item.could_not_rewrite.push_back(rewrite_failure{ file, std::move(code), std::move(reason) });
			for (const asset_usage* usage : group) {
				if (!usage->by_guid) item.broken_references.push_back(*usage);
			}
		};
		fs::path root;
		std::string relative;
		if (group.front()->kind == usage_kind::manifest) {
			if (!manifest) {
				reject("unknown_file", "the project manifest is not known");
				continue;
			}
			root = manifest->root;
			relative = manifest->file;
		} else {
			const asset_record* holder = db->find_any_case(res::tag{ file });
			if (!holder) {
				reject("unknown_file", "the asset database has no such file any more");
				continue;
			}
			if (is_engine_layer(layers, holder->layer, m_ports.engine_base)) {
				reject("engine_layer", "a file of the engine is not changed");
				continue;
			}
			root = layers[holder->layer];
			relative = std::string(holder->path.relative());
		}
		std::vector<std::byte> bytes;
		if (const std::string error = read_file(root, relative, k_level_file_limit, bytes); !error.empty()) {
			reject("unreadable", error);
			continue;
		}
		std::vector<std::string> pointers;
		pointers.reserve(group.size());
		for (const asset_usage* usage : group) pointers.push_back(usage->json_pointer);
		const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		rewrite_outcome outcome = rewrite_references(text, pointers, record->path.view(), item.target.view(), db->case_insensitive());
		if (outcome.status == rewrite_status::nothing_to_do) continue;
		if (outcome.status != rewrite_status::rewritten) {
			reject(std::string(rewrite_status_name(outcome.status)), std::move(outcome.reason));
			continue;
		}
		item.rewrites.push_back(planned_rewrite{ file, std::move(root), std::move(relative), hash_of(bytes), std::move(outcome.text), std::move(outcome.pointers) });
	}
	return std::nullopt;
}

edt::operation_report edt::asset_ops::execute(const operation_plan& plan, const execute_options& options) const
{
	operation_report report;
	const auto refuse = [&report](refusal why) {
		report.refused = std::move(why);
		return report;
	};
	if (plan.refused) return refuse(*plan.refused);
	if (plan.items.size() != 1) return refuse(stop(refusal_code::one_asset_only, "one asset at a time"));
	const plan_item& item = plan.items.front();
	const bool removing = item.request.operation == asset_operation::remove;
	report.warnings = item.warnings;
	report.could_not_rewrite = item.could_not_rewrite;
	report.broken_references = item.broken_references;

	if (removing) {
		if (item.scan_problem) {
			if (!options.force) return refuse(*item.scan_problem);
			report.warnings.push_back(std::format("forced: {}", item.scan_problem->reason));
		}
		if (item.needs_force) {
			if (!options.force) {
				refusal why = stop(refusal_code::has_referrers, std::format("{} place(s) refer to '{}'", item.referrers.size(), item.request.asset.view()));
				why.referrers = item.referrers;
				return refuse(std::move(why));
			}
			report.broken_references = item.referrers;
			report.warnings.push_back(std::format("'{}' was removed while {} place(s) still refer to it: they are left as they are", item.request.asset.view(), item.referrers.size()));
		}
	}
	if (m_ports.journal_root.empty()) return refuse(stop(refusal_code::io_error, "there is no folder for the operation journal"));

	// Nothing is touched when a file is not what the plan read.
	std::vector<std::vector<std::byte>> originals(item.rewrites.size());
	for (const planned_move& move : item.moves) {
		std::string error;
		if (hash_file(item.root, move.from, error) != move.hash || !error.empty()) {
			return refuse(stop(refusal_code::stale_plan, std::format("'{}' changed since it was planned", res::tag::make(move.from).view())));
		}
		if (!removing && exists_on_disk(item.root, move.to)) {
			return refuse(stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(move.to).view())));
		}
	}
	for (std::size_t i = 0; i < item.rewrites.size(); ++i) {
		const planned_rewrite& rewrite = item.rewrites[i];
		const std::string error = read_file(rewrite.root, rewrite.relative, k_level_file_limit, originals[i]);
		if (!error.empty() || hash_of(originals[i]) != rewrite.before_hash) {
			return refuse(stop(refusal_code::stale_plan, std::format("'{}' changed since it was planned", rewrite.file)));
		}
	}

	operation_journal& journal = report.journal;
	journal.id = res::to_text(res::generate_asset_id());
	journal.operation = item.request.operation;
	const fs::path directory = m_ports.journal_root / journal.id;
	journal.directory = utf8(directory);
	report.operation_id = journal.id;
	std::vector<std::string> original_names(item.rewrites.size());
	for (std::size_t i = 0; i < item.rewrites.size(); ++i) {
		original_names[i] = std::format("originals/{}", i);
		if (const std::string error = write_plain(directory / path_of(original_names[i]), originals[i]); !error.empty()) {
			return refuse(stop(refusal_code::io_error, std::format("the original of '{}' was not kept: {}", item.rewrites[i].file, error)));
		}
	}
	originals.clear();

	const std::shared_ptr<void> hold = m_ports.hold_changes ? m_ports.hold_changes() : nullptr;
	const std::string root = utf8(item.root);
	const auto injected = [this](const std::string& label) { return m_ports.fail_step ? m_ports.fail_step(label) : std::string{}; };

	const auto run = [&]() -> std::string {
		for (const planned_move& move : item.moves) {
			if (removing) {
				if (std::string error = injected("stash:" + move.from); !error.empty()) return error;
				std::vector<std::byte> bytes;
				if (std::string error = read_file(item.root, move.from, k_max_moved_bytes, bytes); !error.empty()) return error;
				if (hash_of(bytes) != move.hash) return std::format("'{}' changed while it was removed", move.from);
				if (std::string error = write_plain(directory / path_of(move.to), bytes); !error.empty()) return error;
				if (std::string error = remove_file(item.root, move.from); !error.empty()) return error;
				journal.steps.push_back(journal_step{ journal_step_kind::stash, root, move.from, move.to, move.hash, {}, {} });
			} else {
				if (std::string error = injected("move:" + move.from); !error.empty()) return error;
				if (std::string error = move_file(item.root, move.from, move.to); !error.empty()) return error;
				journal.steps.push_back(journal_step{ journal_step_kind::move, root, move.from, move.to, move.hash, {}, {} });
			}
			persist(journal);
		}
		for (std::size_t i = 0; i < item.rewrites.size(); ++i) {
			const planned_rewrite& rewrite = item.rewrites[i];
			if (std::string error = injected("rewrite:" + rewrite.relative); !error.empty()) return error;
			const std::span<const std::byte> bytes = std::as_bytes(std::span<const char>(rewrite.after_text.data(), rewrite.after_text.size()));
			if (std::string error = write_file(rewrite.root, rewrite.relative, bytes, true); !error.empty()) return error;
			journal.steps.push_back(journal_step{ journal_step_kind::rewrite, utf8(rewrite.root), rewrite.relative, {}, rewrite.before_hash,
				hash_of(rewrite.after_text), original_names[i] });
			persist(journal);
		}
		return {};
	};

	if (std::string error = run(); !error.empty()) {
		for (auto step = journal.steps.rbegin(); step != journal.steps.rend(); ++step) {
			if (std::string undone = undo_step(*step, directory); !undone.empty()) {
				report.rollback_failures.push_back(std::format("{}: {}", step_label(*step), undone));
				egLOG_WARN("asset_ops", "'{}' could not be taken back: {}", step_label(*step), undone);
			}
		}
		report.error = std::move(error);
		if (report.rollback_failures.empty()) journal.steps.clear();
		persist(journal);
		return report;
	}

	for (const planned_move& move : item.moves) {
		const std::string to = removing ? std::format("cache://asset_ops/{}/{}", journal.id, move.to) : std::string(res::tag::make(move.to).view());
		report.moved.push_back(file_move{ std::string(res::tag::make(move.from).view()), to });
	}
	for (const planned_rewrite& rewrite : item.rewrites) report.rewritten.push_back(rewritten_file{ rewrite.file, rewrite.pointers });
	report.ok = true;
	return report;
}

edt::undo_report edt::asset_ops::undo(const operation_journal& journal) const
{
	undo_report report;
	const fs::path directory = path_of(journal.directory);
	for (const journal_step& step : journal.steps) {
		const fs::path root = path_of(step.root);
		std::string error;
		switch (step.kind) {
		case journal_step_kind::move:
			if (hash_file(root, step.to, error) != step.hash || !error.empty() || exists_on_disk(root, step.from)) report.changed_since.push_back(step.to);
			break;
		case journal_step_kind::rewrite:
			if (hash_file(root, step.from, error) != step.after_hash || !error.empty()) report.changed_since.push_back(step.from);
			break;
		case journal_step_kind::stash:
			if (exists_on_disk(root, step.from)) report.changed_since.push_back(step.from);
			if (hash_plain(directory / path_of(step.to), error) != step.hash || !error.empty()) report.changed_since.push_back(step.to);
			break;
		}
	}
	if (!report.changed_since.empty()) {
		report.error = "files changed since the operation, so nothing was put back";
		return report;
	}

	const std::shared_ptr<void> hold = m_ports.hold_changes ? m_ports.hold_changes() : nullptr;
	for (auto step = journal.steps.rbegin(); step != journal.steps.rend(); ++step) {
		if (std::string error = undo_step(*step, directory); !error.empty()) {
			report.failures.push_back(std::format("{}: {}", step_label(*step), error));
			egLOG_WARN("asset_ops", "'{}' could not be put back: {}", step_label(*step), error);
			continue;
		}
		report.restored.push_back(step->from);
	}
	report.ok = report.failures.empty();
	if (!report.ok) report.error = "some files could not be put back";
	return report;
}
