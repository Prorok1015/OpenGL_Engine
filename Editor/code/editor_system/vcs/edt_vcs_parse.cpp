#include "edt_vcs_parse.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <system_error>
#include <unordered_set>

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
	constexpr std::size_t fields_per_commit = 7;
	std::vector<commit_entry> out;
	const auto fields = split_keep_empty(output, '\0');
	for (std::size_t i = 0; i + fields_per_commit <= fields.size(); i += fields_per_commit) {
		commit_entry commit;
		commit.hash = trim(fields[i]);
		// Not a full lowercase object name: the output is not what the format asked for (another encoding, a cut).
		const bool full_oid = (commit.hash.size() == 40 || commit.hash.size() == 64)
			&& std::all_of(commit.hash.begin(), commit.hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		if (!full_oid) continue;
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
		commit.refs = parse_decorations(fields[i + 6]);
		out.push_back(std::move(commit));
	}
	return out;
}

json::value edt::vcs::to_json(const commit_entry& commit)
{
	json::array parents;
	for (const auto& parent : commit.parents) parents.push_back(json::string(parent));
	json::array refs;
	refs.reserve(commit.refs.size());
	for (const auto& ref : commit.refs) refs.push_back(json::object{ { "name", text_of(ref.name) }, { "kind", ref_kind_name(ref.kind) } });
	return json::object{
		{ "hash", json::string(commit.hash) },
		{ "parents", std::move(parents) },
		{ "author", text_of(commit.author) },
		{ "email", text_of(commit.email) },
		{ "time", commit.time },
		{ "subject", text_of(commit.subject) },
		{ "refs", std::move(refs) } };
}

const char* edt::vcs::ref_kind_name(ref_kind kind)
{
	switch (kind) {
	case ref_kind::head: return "head";
	case ref_kind::branch: return "branch";
	case ref_kind::remote: return "remote";
	case ref_kind::tag: return "tag";
	}
	return "branch";
}

std::vector<edt::vcs::decoration> edt::vcs::parse_decorations(std::string_view text)
{
	constexpr std::string_view head_arrow = "HEAD -> ";
	constexpr std::string_view tag_prefix = "tag: ";
	constexpr std::string_view heads = "refs/heads/";
	constexpr std::string_view remotes = "refs/remotes/";
	constexpr std::string_view tags = "refs/tags/";
	std::vector<decoration> out;
	while (!text.empty()) {
		const std::size_t comma = text.find(", ");
		std::string_view item = trim(text.substr(0, comma));
		text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 2);
		if (item.starts_with(head_arrow)) {
			out.push_back({ "HEAD", ref_kind::head });
			item.remove_prefix(head_arrow.size());
		} else if (item == "HEAD") {
			out.push_back({ "HEAD", ref_kind::head });
			continue;
		}
		const bool tagged = item.starts_with(tag_prefix);
		if (tagged) item.remove_prefix(tag_prefix.size());
		decoration ref;
		if (item.starts_with(tags)) {
			ref = { std::string(item.substr(tags.size())), ref_kind::tag };
		} else if (tagged) {
			ref = { std::string(item), ref_kind::tag };
		} else if (item.starts_with(heads)) {
			ref = { std::string(item.substr(heads.size())), ref_kind::branch };
		} else if (item.starts_with(remotes)) {
			ref = { std::string(item.substr(remotes.size())), ref_kind::remote };
			if (ref.name.ends_with("/HEAD")) continue;
		} else if (item.starts_with("refs/") || item == "grafted" || item == "replaced") {
			// refs/stash, refs/notes, ...; a shallow or replaced commit's marker.
			continue;
		} else {
			ref = { std::string(item), ref_kind::branch };
		}
		if (!ref.name.empty()) out.push_back(std::move(ref));
	}
	return out;
}

bool edt::vcs::is_hex_object_name(std::string_view text)
{
	if (text.size() < 4 || text.size() > 64) return false;
	return std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

edt::vcs::tip_list edt::vcs::parse_tips(std::string_view output)
{
	const auto full_oid = [](std::string_view oid) { return (oid.size() == 40 || oid.size() == 64) && is_hex_object_name(oid); };
	tip_list out;
	std::unordered_set<std::string_view> seen;
	for (std::string_view line : split_keep_empty(output, '\n')) {
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		const auto fields = split_keep_empty(line, ' ');
		if (fields.size() < 2) continue;
		std::string_view commit;
		if (fields[1] == "commit") {
			commit = fields[0];
		} else if (fields[1] == "tag" && fields.size() >= 4 && fields[3] == "commit") {
			commit = fields[2];
		} else if (fields[1] == "tag" && fields.size() >= 4 && fields[3] == "tag") {
			// One level of %(*...) peels only to the next tag: git peels the outer one to its commit.
			if (full_oid(fields[0]) && seen.insert(fields[0]).second) out.nested_tags.emplace_back(fields[0]);
			continue;
		}
		if (!full_oid(commit) || !seen.insert(commit).second) continue;
		out.commits.emplace_back(commit);
	}
	return out;
}

std::vector<edt::vcs::changed_file> edt::vcs::parse_name_status(std::string_view output)
{
	// Only what a NUL closed is whole.
	const std::size_t last = output.rfind('\0');
	output = last == std::string_view::npos ? std::string_view{} : output.substr(0, last + 1);
	const auto fields = split_keep_empty(output, '\0');
	std::vector<changed_file> out;
	std::size_t i = 0;
	while (i < fields.size()) {
		const std::string_view status = fields[i];
		const bool known = !status.empty() && status.front() >= 'A' && status.front() <= 'Z'
			&& std::all_of(status.begin() + 1, status.end(), [](char c) { return c >= '0' && c <= '9'; });
		if (!known) {
			++i;
			continue;
		}
		changed_file file;
		file.status = status.front();
		if (file.status == 'R' || file.status == 'C') {
			if (i + 2 >= fields.size()) break;
			file.orig_path = fields[i + 1];
			file.path = fields[i + 2];
			i += 3;
		} else {
			if (i + 1 >= fields.size()) break;
			file.path = fields[i + 1];
			i += 2;
		}
		if (!file.path.empty()) out.push_back(std::move(file));
	}
	return out;
}

json::value edt::vcs::to_json(const changed_file& file)
{
	json::object out{ { "path", text_of(file.path) }, { "status", json::string(std::string(1, file.status)) } };
	if (!file.orig_path.empty()) out["orig_path"] = text_of(file.orig_path);
	return out;
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

std::optional<edt::vcs::git_progress> edt::vcs::parse_progress(std::string_view line)
{
	constexpr std::string_view remote_prefix = "remote: ";
	constexpr std::string_view clear_to_end = "\x1b[K";
	if (line.starts_with(remote_prefix)) line.remove_prefix(remote_prefix.size());
	line = trim(line);
	// git ends a remote's line with an ANSI clear when its stderr is a terminal, else with spaces.
	if (line.ends_with(clear_to_end)) line = trim(line.substr(0, line.size() - clear_to_end.size()));
	const std::size_t colon = line.find(':');
	if (colon == std::string_view::npos || colon == 0) return std::nullopt;
	const std::string_view phase = line.substr(0, colon);
	const auto in_phase = [](char c) { return c == ' ' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	if (phase.front() < 'A' || phase.front() > 'Z' || phase.back() == ' ' || !std::all_of(phase.begin(), phase.end(), in_phase)) {
		return std::nullopt;
	}
	std::string_view rest = line.substr(colon + 1);
	if (!rest.starts_with(' ')) return std::nullopt;
	rest = trim(rest);
	const std::size_t digits_end = std::min(rest.find_first_not_of("0123456789"), rest.size());
	if (digits_end == 0) return std::nullopt;
	const std::string_view number = rest.substr(0, digits_end);
	rest.remove_prefix(digits_end);
	git_progress out;
	out.phase = std::string(phase);
	if (rest.starts_with('%')) {
		const std::optional<int> percent = int_of(number);
		if (!percent || *percent > 100) return std::nullopt;
		rest.remove_prefix(1);
		if (!rest.starts_with(" (")) return std::nullopt;
		const std::size_t close = rest.find(')');
		if (close == std::string_view::npos) return std::nullopt;
		const std::string_view counts = rest.substr(2, close - 2);
		const std::size_t slash = counts.find('/');
		const auto all_digits = [](std::string_view text) {
			return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
		};
		if (slash == std::string_view::npos || !all_digits(counts.substr(0, slash)) || !all_digits(counts.substr(slash + 1))) {
			return std::nullopt;
		}
		out.percent = *percent;
		rest.remove_prefix(close + 1);
	}
	if (!rest.empty() && !rest.starts_with(", ")) return std::nullopt;
	return out;
}

float edt::vcs::progress_fraction(const git_progress& progress)
{
	if (!progress.percent) return 0.f;
	const float share = static_cast<float>(*progress.percent) / 100.f;
	const std::string_view phase = progress.phase;
	const auto one_of = [phase](std::initializer_list<std::string_view> names) {
		return std::find(names.begin(), names.end(), phase) != names.end();
	};
	if (one_of({ "Enumerating objects", "Counting objects", "Compressing objects" })) return 0.1f * share;
	if (one_of({ "Receiving objects", "Unpacking objects", "Writing objects" })) return 0.1f + 0.8f * share;
	if (one_of({ "Resolving deltas", "Checking connectivity", "Checking objects", "Updating files" })) return 0.9f + 0.1f * share;
	return 0.f;
}
