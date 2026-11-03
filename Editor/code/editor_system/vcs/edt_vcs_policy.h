#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What a guarded git write (one not from a person) may run of the repository's config. Off: hooks point at an empty
	// folder of the editor; stage, unstage, commit, checkout and pull are refused with filters_require_opt_in when a
	// configured filter driver applies to a file in the index or one they touch (switching the driver off instead would
	// commit LFS files as raw bytes); fetch, pull and push are refused with unsafe_repository when a credential helper
	// comes from the repository's own config; any key of the repository's own config outside the allowlist refuses.
	struct vcs_write_permissions
	{
		bool allow_hooks = false;
		bool allow_filters = false;
		bool allow_repo_credential_helper = false;
		// Skips only the allowlist of the repository's own config keys; every override and other rule stays.
		bool allow_any_repo_config = false;
	};

	// The rules of the agent policy's "vcs" section (#455) as vcs_service applies them to a guarded write, whatever the
	// mode of its category: a push only to a branch matching push_branches and none of protected_branches, fetch, pull
	// and push only to a remote in remotes. The defaults are what applies without a policy file.
	struct vcs_policy
	{
		// Globs: `*` any run of characters, '/' included, `?` one character; case-sensitive.
		std::vector<std::string> push_branches{ "agent/*" };
		// Remote names, compared exactly.
		std::vector<std::string> remotes{ "origin" };
		// Globs as push_branches, compared without case: `Main` is main.
		std::vector<std::string> protected_branches{ "main", "master" };
		vcs_write_permissions permissions;
	};

	// `*` matches any run of characters (including '/'), `?` exactly one; everything else matches itself (ASCII case
	// folded when `ignore_case`).
	bool vcs_glob_match(std::string_view pattern, std::string_view text, bool ignore_case = false);
}
