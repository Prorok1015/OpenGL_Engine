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

	// The format given to `git log -z --decorate=full`: hash, parents, author name, author email, author time (unix
	// seconds), subject, decorations.
	inline constexpr std::string_view k_log_format = "--format=%H%x00%P%x00%an%x00%ae%x00%at%x00%s%x00%D";

	enum class ref_kind : std::uint8_t { head, branch, remote, tag };
	const char* ref_kind_name(ref_kind kind);

	struct decoration
	{
		// "HEAD", "main", "origin/main", "v1".
		std::string name;
		ref_kind kind = ref_kind::branch;
	};

	// `%D` of `git log --decorate=full`: "HEAD -> refs/heads/main, tag: refs/tags/v1, refs/remotes/origin/main".
	// A remote's symbolic HEAD (refs/remotes/x/HEAD) and refs outside heads, remotes and tags are left out; a short
	// name (no refs/ prefix) is a branch.
	std::vector<decoration> parse_decorations(std::string_view text);

	struct commit_entry
	{
		std::string hash;
		std::vector<std::string> parents;
		std::string author;
		std::string email;
		std::int64_t time = 0;
		std::string subject;
		std::vector<decoration> refs;
	};

	// The output of `git log -z <k_log_format>`: NUL ends each field and each commit, so a commit is seven fields. A
	// record whose hash is not a full lowercase object name (40 or 64 hex digits) is left out.
	std::vector<commit_entry> parse_log(std::string_view output);
	boost::json::value to_json(const commit_entry& commit);

	// The format given to `git for-each-ref` for the tips of a log over every branch, remote branch and tag.
	inline constexpr std::string_view k_tip_format = "--format=%(objectname) %(objecttype) %(*objectname) %(*objecttype)";

	struct tip_list
	{
		// The commit each ref names (a tag peeled to its commit), each once, in order.
		std::vector<std::string> commits;
		// Tags whose first peel is a tag again: to be peeled to a commit by git (rev-parse <tag>^{commit}).
		std::vector<std::string> nested_tags;
	};

	// `git for-each-ref <k_tip_format>`. A ref to anything else (a tag of a tree) is left out, and so is a line without
	// a full hex object name.
	tip_list parse_tips(std::string_view output);

	// 4..64 hex digits: an abbreviated or full object name, never an option or a revision expression.
	bool is_hex_object_name(std::string_view text);

	struct changed_file
	{
		std::string path;
		// Renamed or copied: the path it came from.
		std::string orig_path;
		// git's letter: A, M, D, T, R, C, U, X (the score of R and C is dropped).
		char status = 'M';
	};

	// `git diff-tree -r -z --name-status`: "M", NUL, path, NUL; a rename or copy "R100", NUL, old, NUL, new, NUL. An
	// incomplete last record (output cut short) is left out.
	std::vector<changed_file> parse_name_status(std::string_view output);
	boost::json::value to_json(const changed_file& file);

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

	// One update of what `--progress` prints on stderr: "Receiving objects:  45% (450/1000), 1.2 MiB | 3 MiB/s",
	// "remote: Resolving deltas: 100% (10/10), done.", a counter without a total "Enumerating objects: 5, done.".
	struct git_progress
	{
		// "Receiving objects", without the "remote: " prefix.
		std::string phase;
		// 0..100; none for a counter without a total.
		std::optional<int> percent;
	};

	// Nullopt for a line that is not a progress update (git's other messages, garbage, empty).
	std::optional<git_progress> parse_progress(std::string_view line);
	// Where an update stands in a whole fetch or push, in [0, 1]: counting and compressing take [0, 0.1], the
	// transfer (receiving, unpacking, writing objects) [0.1, 0.9], resolving deltas and checks [0.9, 1]; another
	// phase or one without a percent counts 0.
	float progress_fraction(const git_progress& progress);
}
