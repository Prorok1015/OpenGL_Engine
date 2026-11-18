#include "edt_asset_history.h"
#include "edt_forge_types.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_stamp.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <set>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::string_view k_journal_file = "journal.json";
	constexpr std::size_t k_max_listed_files = 5;
	constexpr std::uintmax_t k_max_journal_bytes = std::uintmax_t{ 16 } << 20;
	constexpr std::uintmax_t k_max_list_bytes = std::uintmax_t{ 1 } << 20;
	constexpr int k_list_version = 1;

	std::string hash_of_journal(const edt::operation_journal& journal)
	{
		const std::string text = json::serialize(edt::journal_to_json(journal));
		res::content_hasher hasher;
		hasher.update(std::as_bytes(std::span<const char>(text.data(), text.size())));
		return hasher.text();
	}

	std::optional<std::string> read_text(const fs::path& file, std::uintmax_t limit)
	{
		std::error_code ec;
		if (!fs::is_regular_file(file, ec)) return std::nullopt;
		const std::uintmax_t size = fs::file_size(file, ec);
		if (ec || size > limit) return std::nullopt;
		std::ifstream in(file, std::ios::binary);
		if (!in) return std::nullopt;
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	std::string name_of(std::string_view relative)
	{
		const std::size_t slash = relative.find_last_of('/');
		return std::string(slash == std::string_view::npos ? relative : relative.substr(slash + 1));
	}

	// The file the operation was asked about: the first one that was moved or deleted and is neither a .resource nor a desc's binary
	// file, so a pair is named by its desc, whatever order its steps are in. A referrer that was rewritten is never it.
	std::string primary_file_of(const edt::operation_journal& journal)
	{
		const auto moved = [](const edt::journal_step& step) { return step.kind != edt::journal_step_kind::rewrite && !step.from.starts_with("cache://"); };
		const auto main_name = [](const edt::journal_step& step) {
			const std::string name = name_of(step.from);
			return !res::is_asset_meta_name(name) && !res::desc_binary_owner_name(name);
		};
		const auto main = std::ranges::find_if(journal.steps, [&](const edt::journal_step& step) { return moved(step) && main_name(step); });
		if (main != journal.steps.end()) return main->from;
		const auto any_moved = std::ranges::find_if(journal.steps, moved);
		return (any_moved != journal.steps.end() ? *any_moved : journal.steps.front()).from;
	}

	std::string listed(const std::vector<std::string>& files)
	{
		std::string out;
		for (std::size_t i = 0; i < files.size() && i < k_max_listed_files; ++i) {
			if (i > 0) out += ", ";
			out += edt::forge::escaped_text(files[i], 128);
		}
		if (files.size() > k_max_listed_files) out += std::format(" and {} more", files.size() - k_max_listed_files);
		return out;
	}

	bool same_folder(const fs::path& a, const fs::path& b)
	{
		std::error_code ec;
		const fs::path first = fs::weakly_canonical(a, ec);
		const fs::path second = ec ? fs::path{} : fs::weakly_canonical(b, ec);
		return ec ? a.lexically_normal() == b.lexically_normal() : first == second;
	}
}

std::string edt::asset_operation_label(const operation_journal& journal)
{
	const char* verb = "Rename";
	switch (journal.operation) {
	case asset_operation::rename: verb = "Rename"; break;
	case asset_operation::move: verb = "Move"; break;
	case asset_operation::remove: verb = "Delete"; break;
	}
	if (journal.steps.empty()) return std::string(verb) + " asset";
	return std::format("{} {}", verb, forge::escaped_text(name_of(primary_file_of(journal)), 128));
}

fs::path edt::asset_history_file(const std::vector<fs::path>& edt_roots, const fs::path& user_data_dir, const fs::path& journal_root)
{
	if (journal_root.empty()) return {};
	const std::string where = fs_paths::generic_utf8_of(journal_root.lexically_normal());
	res::content_hasher hasher;
	hasher.update(std::as_bytes(std::span<const char>(where.data(), where.size())));
	const std::string text = hasher.text();
	const std::size_t colon = text.find(':');
	return (edt_roots.empty() ? user_data_dir : edt_roots.back()) / "settings" / "asset_history" / (text.substr(colon == std::string::npos ? 0 : colon + 1) + ".json");
}

edt::asset_history::asset_history(fs::path journal_root, fs::path list_file, std::function<std::uint64_t()> serial)
	: m_root(std::move(journal_root))
	, m_list_file(std::move(list_file))
	, m_serial(std::move(serial))
{
}

void edt::asset_history::save() const
{
	if (m_list_file.empty()) return;
	json::array items;
	for (const entry& item : m_entries) items.push_back(json::object{ { "id", item.journal.id }, { "label", item.label }, { "hash", item.hash } });
	const std::string text = json::serialize(json::object{ { "version", k_list_version }, { "journal_root", fs_paths::generic_utf8_of(m_root.lexically_normal()) },
		{ "entries", std::move(items) } });
	std::error_code ec;
	fs::create_directories(m_list_file.parent_path(), ec);
	if (ec) {
		egLOG_WARN("asset_ops", "the list of the asset operations was not saved: {}", ec.message());
		return;
	}
	const fs::path temporary = m_list_file.parent_path() / (fs_paths::utf8_of(m_list_file.filename()) + ".tmp");
	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		out.flush();
		if (!out) {
			out.close();
			fs::remove(temporary, ec);
			egLOG_WARN("asset_ops", "the list of the asset operations was not saved: it cannot be written");
			return;
		}
	}
	fs::rename(temporary, m_list_file, ec);
	if (ec) {
		fs::remove(temporary, ec);
		egLOG_WARN("asset_ops", "the list of the asset operations was not saved: {}", ec.message());
	}
}

bool edt::asset_history::inside_root(const fs::path& directory) const
{
	return !m_root.empty() && same_folder(directory.parent_path(), m_root);
}

std::size_t edt::asset_history::restore(const std::vector<fs::path>& roots)
{
	m_entries.clear();
	std::error_code ec;
	if (m_root.empty()) return 0;

	struct listed_entry
	{
		std::string id;
		std::string label;
		std::string hash;
	};
	std::vector<listed_entry> from_list;
	if (!m_list_file.empty()) {
		if (const std::optional<std::string> text = read_text(m_list_file, k_max_list_bytes)) {
			json::error_code parse_ec;
			const json::value parsed = json::parse(*text, parse_ec);
			const json::object* object = parse_ec ? nullptr : parsed.if_object();
			const json::value* entries = object ? object->if_contains("entries") : nullptr;
			const json::value* where = object ? object->if_contains("journal_root") : nullptr;
			if (!entries || !entries->is_array() || !where || !where->is_string()) {
				egLOG_WARN("asset_ops", "the list of the asset operations '{}' is not readable and is ignored", fs_paths::generic_utf8_of(m_list_file));
			} else if (std::string_view(where->as_string().data(), where->as_string().size()) != fs_paths::generic_utf8_of(m_root.lexically_normal())) {
				egLOG_WARN("asset_ops", "the list of the asset operations '{}' is of another project and is ignored", fs_paths::generic_utf8_of(m_list_file));
			} else {
				const auto field = [](const json::object& from, std::string_view key) -> std::string {
					const json::value* found = from.if_contains(json::string_view(key.data(), key.size()));
					return found && found->is_string() ? std::string(found->as_string().data(), found->as_string().size()) : std::string{};
				};
				for (const json::value& item : entries->as_array()) {
					const json::object* one = item.if_object();
					if (!one) continue;
					from_list.push_back(listed_entry{ field(*one, "id"), field(*one, "label"), field(*one, "hash") });
				}
			}
		}
	}
	std::set<std::string> known;
	for (const listed_entry& item : from_list) known.insert(item.id);

	// Journals the editor did not list are somebody else's: they are never undone.
	if (fs::is_directory(m_root, ec)) {
		for (fs::directory_iterator it(m_root, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code entry_ec;
			const std::string name = fs_paths::utf8_of(it->path().filename());
			if (!it->is_directory(entry_ec) || !fs::is_regular_file(it->path() / std::string(k_journal_file), entry_ec) || known.contains(name)) continue;
			egLOG_WARN("asset_ops", "the journal '{}' is not one the editor made (it is not in the editor's own list) and is ignored", fs_paths::generic_utf8_of(it->path() / std::string(k_journal_file)));
		}
	}

	std::vector<std::string> allowed;
	allowed.reserve(roots.size());
	for (const fs::path& root : roots) allowed.push_back(fs_paths::generic_utf8_of(root));

	std::vector<entry> kept;
	for (const listed_entry& item : from_list) {
		const fs::path file = m_root / item.id / std::string(k_journal_file);
		const std::optional<std::string> text = item.id.empty() || item.id.find_first_of("/\\") != std::string::npos || item.id == ".." || item.id == "."
			? std::nullopt : read_text(file, k_max_journal_bytes);
		if (!text) continue;
		json::error_code parse_ec;
		const json::value parsed = json::parse(*text, parse_ec);
		if (parse_ec) {
			egLOG_WARN("asset_ops", "the journal '{}' is not JSON and is ignored", fs_paths::generic_utf8_of(file));
			continue;
		}
		std::string reason;
		std::optional<operation_journal> journal = journal_from_json(parsed, reason);
		if (!journal) {
			egLOG_WARN("asset_ops", "the journal '{}' is not trusted and is ignored: {}", fs_paths::generic_utf8_of(file), reason);
			continue;
		}
		if (journal->steps.empty() || journal->id != item.id) continue;
		if (hash_of_journal(*journal) != item.hash) {
			egLOG_WARN("asset_ops", "the journal '{}' is not what the editor listed and is ignored", fs_paths::generic_utf8_of(file));
			continue;
		}
		const bool known_roots = std::ranges::all_of(journal->steps, [&allowed](const journal_step& step) {
			return std::ranges::find(allowed, step.root) != allowed.end();
		});
		if (!known_roots) {
			egLOG_WARN("asset_ops", "the journal '{}' names a folder that is not the project's own folder and is ignored", fs_paths::generic_utf8_of(file));
			continue;
		}
		// The folder asset_ops::undo expects, whatever the journal says it was written to.
		journal->directory = fs_paths::generic_utf8_of(m_root / journal->id);
		kept.push_back(entry{ 0, forge::escaped_text(item.label.empty() ? asset_operation_label(*journal) : item.label, 128), item.hash, 0, std::move(*journal) });
	}
	const std::size_t first = kept.size() > max_entries ? kept.size() - max_entries : 0;
	for (std::size_t i = first; i < kept.size(); ++i) {
		kept[i].serial = m_serial ? m_serial() : static_cast<std::uint64_t>(i);
		m_entries.push_back(std::move(kept[i]));
	}
	if (m_entries.size() != from_list.size()) save();
	return m_entries.size();
}

void edt::asset_history::record(operation_journal journal)
{
	if (m_entries.size() >= max_entries) m_entries.erase(m_entries.begin());
	std::string label = asset_operation_label(journal);
	std::string hash = hash_of_journal(journal);
	m_entries.push_back(entry{ m_serial ? m_serial() : static_cast<std::uint64_t>(m_entries.size()), std::move(label), std::move(hash), 0, std::move(journal) });
	save();
}

std::optional<std::string> edt::asset_history::next_label() const
{
	if (m_entries.empty()) return std::nullopt;
	return m_entries.back().label;
}

std::optional<std::uint64_t> edt::asset_history::next_serial() const
{
	if (m_entries.empty()) return std::nullopt;
	return m_entries.back().serial;
}

std::optional<std::string> edt::asset_history::next_operation_id() const
{
	if (m_entries.empty()) return std::nullopt;
	return m_entries.back().journal.id;
}

void edt::asset_history::set_aside(const operation_journal& journal) const
{
	const fs::path directory = fs_paths::path_from_utf8(journal.directory);
	if (!inside_root(directory)) return;
	std::error_code ec;
	fs::rename(directory / std::string(k_journal_file), directory / std::string(dropped_journal_file), ec);
	if (ec) egLOG_WARN("asset_ops", "the journal of '{}' could not be set aside: {}", journal.id, ec.message());
}

edt::asset_fix_result edt::asset_history::undo_last(const asset_ops& operations)
{
	if (m_entries.empty()) return asset_fix_result{ false, "there is no asset operation to undo" };
	const operation_journal journal = m_entries.back().journal;
	const std::string label = m_entries.back().label;

	const undo_report undone = operations.undo(journal);
	if (undone.ok) {
		m_entries.pop_back();
		save();
		const fs::path directory = fs_paths::path_from_utf8(journal.directory);
		if (inside_root(directory)) {
			std::error_code ec;
			fs::remove_all(directory, ec);
			if (ec) egLOG_WARN("asset_ops", "the journal folder of '{}' was not deleted: {}", journal.id, ec.message());
		}
		return asset_fix_result{ true, {} };
	}
	std::string why = std::format("'{}' was not undone: {}", label, undone.error);
	if (!undone.changed_since.empty()) why += std::format(" ({})", listed(undone.changed_since));
	if (!undone.open_with_unsaved_changes.empty()) why += std::format(" ({})", listed(undone.open_with_unsaved_changes));
	if (!undone.failures.empty()) why += std::format(" ({})", listed(undone.failures));
	if (++m_entries.back().attempts >= max_undo_attempts) {
		m_entries.pop_back();
		save();
		set_aside(journal);
		why += "; it failed too often and is dropped from the history";
		return asset_fix_result{ false, std::move(why), false };
	}
	why += "; it stays in the history: fix that and undo again";
	return asset_fix_result{ false, std::move(why), true };
}
