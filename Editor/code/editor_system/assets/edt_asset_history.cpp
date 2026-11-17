#include "edt_asset_history.h"
#include "edt_forge_types.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::string_view k_journal_file = "journal.json";
	constexpr std::size_t k_max_listed_files = 5;
	constexpr std::uintmax_t k_max_journal_bytes = std::uintmax_t{ 16 } << 20;

	std::string name_of(std::string_view relative)
	{
		const std::size_t slash = relative.find_last_of('/');
		return std::string(slash == std::string_view::npos ? relative : relative.substr(slash + 1));
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
	return std::format("{} {}", verb, forge::escaped_text(name_of(journal.steps.front().from), 128));
}

edt::asset_history::asset_history(fs::path journal_root, std::function<std::uint64_t()> serial)
	: m_root(std::move(journal_root))
	, m_serial(std::move(serial))
{
}

bool edt::asset_history::inside_root(const fs::path& directory) const
{
	return !m_root.empty() && same_folder(directory.parent_path(), m_root);
}

std::size_t edt::asset_history::restore(const std::vector<fs::path>& roots)
{
	m_entries.clear();
	std::error_code ec;
	if (m_root.empty() || !fs::is_directory(m_root, ec)) return 0;

	std::vector<std::string> allowed;
	allowed.reserve(roots.size());
	for (const fs::path& root : roots) allowed.push_back(fs_paths::generic_utf8_of(root));

	struct found
	{
		fs::file_time_type written;
		operation_journal journal;
	};
	std::vector<found> journals;
	for (fs::directory_iterator it(m_root, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code entry_ec;
		const fs::path file = it->path() / std::string(k_journal_file);
		if (!it->is_directory(entry_ec) || !fs::is_regular_file(file, entry_ec)) continue;
		const std::uintmax_t size = fs::file_size(file, entry_ec);
		if (entry_ec || size > k_max_journal_bytes) continue;
		std::ifstream in(file, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		json::error_code parse_ec;
		const json::value parsed = json::parse(text, parse_ec);
		if (parse_ec) {
			egLOG_WARN("asset_ops", "the journal '{}' is not JSON and is ignored", fs_paths::generic_utf8_of(file));
			continue;
		}
		std::optional<operation_journal> journal = journal_from_json(parsed);
		if (!journal || journal->steps.empty()) continue;
		const bool known_roots = std::ranges::all_of(journal->steps, [&allowed](const journal_step& step) {
			return std::ranges::find(allowed, step.root) != allowed.end();
		});
		if (!known_roots) {
			egLOG_WARN("asset_ops", "the journal '{}' names a folder that is not the project's and is ignored", fs_paths::generic_utf8_of(file));
			continue;
		}
		// Where it was found, not where it says it was written: a project folder that moved keeps its journals.
		journal->directory = fs_paths::generic_utf8_of(it->path());
		const fs::file_time_type written = fs::last_write_time(file, entry_ec);
		journals.push_back(found{ entry_ec ? fs::file_time_type{} : written, std::move(*journal) });
	}
	std::ranges::stable_sort(journals, {}, &found::written);
	const std::size_t first = journals.size() > max_entries ? journals.size() - max_entries : 0;
	for (std::size_t i = first; i < journals.size(); ++i) {
		m_entries.push_back(entry{ m_serial ? m_serial() : static_cast<std::uint64_t>(i), asset_operation_label(journals[i].journal), std::move(journals[i].journal) });
	}
	return m_entries.size();
}

void edt::asset_history::record(operation_journal journal)
{
	if (m_entries.size() >= max_entries) m_entries.erase(m_entries.begin());
	std::string label = asset_operation_label(journal);
	m_entries.push_back(entry{ m_serial ? m_serial() : static_cast<std::uint64_t>(m_entries.size()), std::move(label), std::move(journal) });
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
	entry taken = std::move(m_entries.back());
	m_entries.pop_back();

	const undo_report undone = operations.undo(taken.journal);
	if (undone.ok) {
		const fs::path directory = fs_paths::path_from_utf8(taken.journal.directory);
		if (inside_root(directory)) {
			std::error_code ec;
			fs::remove_all(directory, ec);
			if (ec) egLOG_WARN("asset_ops", "the journal folder of '{}' was not deleted: {}", taken.journal.id, ec.message());
		}
		return asset_fix_result{ true, {} };
	}
	std::string why = std::format("'{}' was not undone: {}", taken.label, undone.error);
	if (!undone.changed_since.empty()) why += std::format(" ({})", listed(undone.changed_since));
	if (!undone.failures.empty()) why += std::format(" ({})", listed(undone.failures));
	set_aside(taken.journal);
	return asset_fix_result{ false, std::move(why) };
}
