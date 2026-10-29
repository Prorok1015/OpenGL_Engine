#pragma once
#include <boost/json/value.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pure parsers of git's machine-readable output (#454). Every text taken from git passes through valid_utf8, so the
// JSON built from it is valid UTF-8 even for a path or a name that is not.
namespace edt::vcs
{
	// `text` with every byte that does not start or continue a valid UTF-8 sequence replaced by U+FFFD.
	std::string valid_utf8(std::string_view text);
	// The longest prefix of `text` of at most `max_bytes` that does not end inside a UTF-8 sequence.
	std::string_view utf8_prefix(std::string_view text, std::size_t max_bytes);

	enum class entry_kind : std::uint8_t { ordinary, renamed, unmerged, untracked, ignored };
	const char* entry_kind_name(entry_kind kind);

	struct status_entry
	{
		// Relative to the repository root, '/' separated.
		std::string path;
		// Renamed or copied entries: the path it came from.
		std::string orig_path;
		// The X and Y of git's XY: '.' unchanged, M, T, A, D, R, C, U; '?' / '!' for untracked / ignored.
		char index = '.';
		char worktree = '.';
		entry_kind kind = entry_kind::ordinary;
	};

	struct branch_state
	{
		// The branch name; empty when detached.
		std::string head;
		bool detached = false;
		// The commit; empty on an unborn branch ("(initial)").
		std::string oid;
		std::string upstream;
		// Only when the upstream exists ("# branch.ab").
		std::optional<int> ahead;
		std::optional<int> behind;
	};

	struct status_report
	{
		branch_state branch;
		std::vector<status_entry> entries;
	};

	// `git status --porcelain=v2 --branch -z`. Records it does not know are skipped.
	status_report parse_status(std::string_view output);
	boost::json::value to_json(const status_report& status);

	// The format given to `git log -z`: hash, parents, author name, author email, author time (unix seconds), subject.
	inline constexpr std::string_view k_log_format = "--format=%H%x00%P%x00%an%x00%ae%x00%at%x00%s";

	struct commit_entry
	{
		std::string hash;
		std::vector<std::string> parents;
		std::string author;
		std::string email;
		std::int64_t time = 0;
		std::string subject;
	};

	// The output of `git log -z <k_log_format>`: NUL ends each field and each commit, so a commit is six fields.
	std::vector<commit_entry> parse_log(std::string_view output);
	boost::json::value to_json(const commit_entry& commit);

	// The format given to `git for-each-ref`: one ref per line (a ref name holds no control character).
	inline constexpr std::string_view k_ref_format = "--format=%(refname)%00%(objectname)%00%(upstream)%00%(HEAD)";

	struct ref_entry
	{
		// "main", "origin/main".
		std::string name;
		// "refs/heads/main".
		std::string full_name;
		std::string oid;
		// Full name of the upstream ("refs/remotes/origin/main"); empty if none.
		std::string upstream;
		bool head = false;
		bool remote = false;
	};

	// `git for-each-ref <k_ref_format> refs/heads refs/remotes`. A remote's symbolic HEAD (refs/remotes/x/HEAD) is
	// left out.
	std::vector<ref_entry> parse_refs(std::string_view output);
	boost::json::value to_json(const ref_entry& ref);

	// What went wrong, from git's stderr: auth_required, unsafe_repository (safe.directory), not_a_repository,
	// no_commits, bad_revision, network, locked, or git_failed when nothing is recognised.
	const char* classify_error(std::string_view stderr_text);

	// `git version 2.43.0` -> "2.43.0"; the trimmed text when it does not look like that.
	std::string parse_version(std::string_view output);
}
