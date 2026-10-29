#include "edt_vcs_parse.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <system_error>

namespace json = boost::json;

namespace {
	// Every piece between NULs, empty ones included; a last empty piece (after a closing NUL) is dropped.
	std::vector<std::string_view> split_keep_empty(std::string_view text, char separator)
	{
		std::vector<std::string_view> out;
		std::size_t start = 0;
		while (start <= text.size()) {
			const std::size_t end = text.find(separator, start);
			if (end == std::string_view::npos) {
				if (start < text.size()) out.push_back(text.substr(start));
				break;
			}
			out.push_back(text.substr(start, end - start));
			start = end + 1;
		}
		return out;
	}

	// The first `count` space-separated fields of `record`, then everything after them (a path may hold spaces).
	bool take_fields(std::string_view record, std::size_t count, std::vector<std::string_view>& fields, std::string_view& rest)
	{
		fields.clear();
		std::size_t start = 0;
		for (std::size_t i = 0; i < count; ++i) {
			const std::size_t end = record.find(' ', start);
			if (end == std::string_view::npos) return false;
			fields.push_back(record.substr(start, end - start));
			start = end + 1;
		}
		rest = record.substr(start);
		return !rest.empty();
	}

	std::optional<int> int_of(std::string_view text)
	{
		if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
			const bool negative = text.front() == '-';
			text.remove_prefix(1);
			int value = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
			return negative ? -value : value;
		}
		int value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
		return value;
	}

	std::string_view trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
		return text;
	}

	json::string text_of(std::string_view raw)
	{
		return json::string(edt::vcs::valid_utf8(raw));
	}

	std::string lower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) {
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		}
		return out;
	}

	bool contains_any(std::string_view text, std::initializer_list<std::string_view> needles)
	{
		return std::any_of(needles.begin(), needles.end(), [text](std::string_view n) { return text.find(n) != std::string_view::npos; });
	}
}

std::string edt::vcs::valid_utf8(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	const auto byte = [&text](std::size_t i) { return static_cast<unsigned char>(text[i]); };
	const auto in = [](unsigned char b, unsigned char lo, unsigned char hi) { return b >= lo && b <= hi; };
	std::size_t i = 0;
	while (i < text.size()) {
		const unsigned char b = byte(i);
		std::size_t length = 0;
		if (b < 0x80) {
			length = 1;
		} else if (in(b, 0xC2, 0xDF)) {
			if (i + 1 < text.size() && in(byte(i + 1), 0x80, 0xBF)) length = 2;
		} else if (in(b, 0xE0, 0xEF)) {
			const unsigned char lo = b == 0xE0 ? 0xA0 : 0x80;
			const unsigned char hi = b == 0xED ? 0x9F : 0xBF;
			if (i + 2 < text.size() && in(byte(i + 1), lo, hi) && in(byte(i + 2), 0x80, 0xBF)) length = 3;
		} else if (in(b, 0xF0, 0xF4)) {
			const unsigned char lo = b == 0xF0 ? 0x90 : 0x80;
			const unsigned char hi = b == 0xF4 ? 0x8F : 0xBF;
			if (i + 3 < text.size() && in(byte(i + 1), lo, hi) && in(byte(i + 2), 0x80, 0xBF) && in(byte(i + 3), 0x80, 0xBF)) length = 4;
		}
		if (length == 0) {
			out += "\xEF\xBF\xBD";
			++i;
		} else {
			out.append(text.substr(i, length));
			i += length;
		}
	}
	return out;
}

std::string_view edt::vcs::utf8_prefix(std::string_view text, std::size_t max_bytes)
{
	if (text.size() <= max_bytes) return text;
	std::size_t cut = max_bytes;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
	return text.substr(0, cut);
}

const char* edt::vcs::entry_kind_name(entry_kind kind)
{
	switch (kind) {
	case entry_kind::ordinary: return "ordinary";
	case entry_kind::renamed: return "renamed";
	case entry_kind::unmerged: return "unmerged";
	case entry_kind::untracked: return "untracked";
	case entry_kind::ignored: return "ignored";
	}
	return "ordinary";
}

edt::vcs::status_report edt::vcs::parse_status(std::string_view output)
{
	status_report out;
	const auto records = split_keep_empty(output, '\0');
	std::vector<std::string_view> fields;
	std::string_view rest;
	for (std::size_t i = 0; i < records.size(); ++i) {
		const std::string_view record = records[i];
		if (record.size() < 2) continue;
		if (record.starts_with("# ")) {
			const std::string_view header = record.substr(2);
			const std::size_t space = header.find(' ');
			if (space == std::string_view::npos) continue;
			const std::string_view key = header.substr(0, space);
			const std::string_view value = header.substr(space + 1);
			if (key == "branch.oid") {
				out.branch.oid = value == "(initial)" ? std::string{} : std::string(value);
			} else if (key == "branch.head") {
				out.branch.detached = value == "(detached)";
				out.branch.head = out.branch.detached ? std::string{} : std::string(value);
			} else if (key == "branch.upstream") {
				out.branch.upstream = value;
			} else if (key == "branch.ab") {
				const std::size_t gap = value.find(' ');
				if (gap == std::string_view::npos) continue;
				const auto ahead = int_of(value.substr(0, gap));
				const auto behind = int_of(value.substr(gap + 1));
				if (ahead && behind) {
					out.branch.ahead = *ahead;
					out.branch.behind = -*behind;
				}
			}
			continue;
		}
		status_entry entry;
		switch (record.front()) {
		case '1':
			if (!take_fields(record, 8, fields, rest) || fields[1].size() != 2) continue;
			entry.kind = entry_kind::ordinary;
			break;
		case '2':
			if (!take_fields(record, 9, fields, rest) || fields[1].size() != 2) continue;
			entry.kind = entry_kind::renamed;
			// -z: the original path is the next record.
			if (i + 1 < records.size()) entry.orig_path = records[++i];
			break;
		case 'u':
			if (!take_fields(record, 10, fields, rest) || fields[1].size() != 2) continue;
			entry.kind = entry_kind::unmerged;
			break;
		case '?':
		case '!':
			if (record[1] != ' ' || record.size() < 3) continue;
			entry.kind = record.front() == '?' ? entry_kind::untracked : entry_kind::ignored;
			entry.index = entry.worktree = record.front();
			entry.path = record.substr(2);
			out.entries.push_back(std::move(entry));
			continue;
		default:
			continue;
		}
		entry.index = fields[1][0];
		entry.worktree = fields[1][1];
		entry.path = rest;
		out.entries.push_back(std::move(entry));
	}
	return out;
}

json::value edt::vcs::to_json(const status_report& status)
{
	json::object branch{
		{ "head", text_of(status.branch.head) },
		{ "detached", status.branch.detached },
		{ "oid", json::string(status.branch.oid) } };
	if (!status.branch.upstream.empty()) branch["upstream"] = text_of(status.branch.upstream);
	if (status.branch.ahead) branch["ahead"] = *status.branch.ahead;
	if (status.branch.behind) branch["behind"] = *status.branch.behind;

	json::array entries;
	entries.reserve(status.entries.size());
	bool clean = true;
	for (const auto& entry : status.entries) {
		json::object item{
			{ "path", text_of(entry.path) },
			{ "index", json::string(std::string(1, entry.index)) },
			{ "worktree", json::string(std::string(1, entry.worktree)) },
			{ "kind", entry_kind_name(entry.kind) } };
		if (!entry.orig_path.empty()) item["orig_path"] = text_of(entry.orig_path);
		if (entry.kind != entry_kind::ignored) clean = false;
		entries.push_back(std::move(item));
	}
	return json::object{ { "branch", std::move(branch) }, { "entries", std::move(entries) }, { "clean", clean } };
}

std::vector<edt::vcs::commit_entry> edt::vcs::parse_log(std::string_view output)
{
	constexpr std::size_t fields_per_commit = 6;
	std::vector<commit_entry> out;
	const auto fields = split_keep_empty(output, '\0');
	for (std::size_t i = 0; i + fields_per_commit <= fields.size(); i += fields_per_commit) {
		commit_entry commit;
		commit.hash = trim(fields[i]);
		if (commit.hash.empty()) continue;
		std::string_view parents = fields[i + 1];
		while (!parents.empty()) {
			const std::size_t space = parents.find(' ');
			const std::string_view parent = parents.substr(0, space);
			if (!parent.empty()) commit.parents.emplace_back(parent);
			if (space == std::string_view::npos) break;
			parents.remove_prefix(space + 1);
		}
		commit.author = fields[i + 2];
		commit.email = fields[i + 3];
		std::int64_t time = 0;
		const std::string_view time_text = trim(fields[i + 4]);
		const auto parsed = std::from_chars(time_text.data(), time_text.data() + time_text.size(), time);
		commit.time = parsed.ec == std::errc{} ? time : 0;
		commit.subject = fields[i + 5];
		out.push_back(std::move(commit));
	}
	return out;
}

json::value edt::vcs::to_json(const commit_entry& commit)
{
	json::array parents;
	for (const auto& parent : commit.parents) parents.push_back(json::string(parent));
	return json::object{
		{ "hash", json::string(commit.hash) },
		{ "parents", std::move(parents) },
		{ "author", text_of(commit.author) },
		{ "email", text_of(commit.email) },
		{ "time", commit.time },
		{ "subject", text_of(commit.subject) } };
}

std::vector<edt::vcs::ref_entry> edt::vcs::parse_refs(std::string_view output)
{
	constexpr std::string_view heads = "refs/heads/";
	constexpr std::string_view remotes = "refs/remotes/";
	std::vector<ref_entry> out;
	for (std::string_view line : split_keep_empty(output, '\n')) {
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		const auto fields = split_keep_empty(line, '\0');
		if (fields.size() < 2) continue;
		ref_entry ref;
		ref.full_name = fields[0];
		if (ref.full_name.starts_with(heads)) {
			ref.name = ref.full_name.substr(heads.size());
		} else if (ref.full_name.starts_with(remotes)) {
			ref.name = ref.full_name.substr(remotes.size());
			ref.remote = true;
			if (ref.name.ends_with("/HEAD")) continue;
		} else {
			continue;
		}
		ref.oid = trim(fields[1]);
		if (fields.size() > 2) ref.upstream = fields[2];
		ref.head = fields.size() > 3 && trim(fields[3]) == "*";
		out.push_back(std::move(ref));
	}
	return out;
}

json::value edt::vcs::to_json(const ref_entry& ref)
{
	json::object out{
		{ "name", text_of(ref.name) },
		{ "full_name", text_of(ref.full_name) },
		{ "oid", json::string(ref.oid) },
		{ "head", ref.head },
		{ "remote", ref.remote } };
	if (!ref.upstream.empty()) out["upstream"] = text_of(ref.upstream);
	return out;
}

const char* edt::vcs::classify_error(std::string_view stderr_text)
{
	const std::string text = lower(stderr_text);
	if (contains_any(text, { "authentication failed", "could not read username", "could not read password",
			"terminal prompts disabled", "permission denied (publickey", "host key verification failed",
			"invalid username or password", "returned error: 401", "returned error: 403" })) {
		return "auth_required";
	}
	if (contains_any(text, { "dubious ownership" })) return "unsafe_repository";
	if (contains_any(text, { "not a git repository" })) return "not_a_repository";
	if (contains_any(text, { "does not have any commits yet", "bad default revision 'head'" })) return "no_commits";
	if (contains_any(text, { "unknown revision", "bad revision", "ambiguous argument", "invalid object name",
			"not a valid object name" })) {
		return "bad_revision";
	}
	if (contains_any(text, { "could not resolve host", "connection refused", "connection timed out", "could not connect",
			"unable to access", "network is unreachable", "could not read from remote repository" })) {
		return "network";
	}
	if (contains_any(text, { ".lock': file exists", "index.lock" })) return "locked";
	return "git_failed";
}

std::string edt::vcs::parse_version(std::string_view output)
{
	constexpr std::string_view prefix = "git version ";
	std::string_view text = trim(output);
	const std::size_t line_end = text.find('\n');
	if (line_end != std::string_view::npos) text = trim(text.substr(0, line_end));
	if (text.starts_with(prefix)) text = trim(text.substr(prefix.size()));
	return valid_utf8(text);
}
