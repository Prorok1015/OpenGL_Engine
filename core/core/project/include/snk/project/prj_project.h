#pragma once
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/project/prj_manifest.h>

namespace prj
{
	// The project's own folders, by their names below its root: never content (project_context names them below `root`).
	inline constexpr std::string_view k_cache_dir_name = ".cache";
	inline constexpr std::string_view k_snk_dir_name = ".snk";
	inline constexpr std::string_view k_code_dir_name = "code";
	inline constexpr std::string_view k_build_dir_name = "build";

	// The top-level folders of any project that are never content: the writable folders all lie inside .cache. Needs no
	// healthy project (a rollback of a broken one checks a journal against them).
	inline std::vector<std::string_view> own_top_dir_names()
	{
		return { k_code_dir_name, k_snk_dir_name, k_cache_dir_name, k_build_dir_name };
	}

	// Why a content root (relative to the project, '/' separated) may not be one, naming the root and the rule; nullopt
	// when it may. Refused (ASCII case folded, so one manifest means the same on every file system): a part starting with
	// '.' anywhere (.git, .github, .snk, assets/.hidden), and a first part that is one of own_top_dir_names() or a folder
	// is_code_ignored_dir names (build output, fetched dependencies). Pure, no file system access (#911).
	SNK_API std::optional<std::string> content_root_error(std::string_view relative);

	// The open project. Lives in app_data_storage from before resource_system (which mounts its content roots)
	// until after it, and is absent when the editor runs without a project.
	struct project_context
	{
		// Canonical directory holding the manifest.
		std::filesystem::path root;
		std::filesystem::path manifest_file;
		manifest data;

		// Absolute, highest priority first (the order of the manifest); every one exists and is inside `root`.
		std::vector<std::filesystem::path> content_roots;

		// The manifest's engine_version against current_engine_version().
		version_match engine_match = version_match::exact;

		// Last, so the fields a game module reads keep their offsets: the manifest's "prefab_limits" (#1031).
		prefab_spawn_limits prefab_limits;

		// <root>/.cache: regenerated data, never committed.
		std::filesystem::path cache_dir() const { return root / std::filesystem::path(k_cache_dir_name); }
		// memory:// (data made at runtime, e.g. imports) inside the cache.
		std::filesystem::path memory_dir() const { return cache_dir() / "memory"; }
		// user:// while the editor runs: a test profile, so the editor never touches the player's real data.
		std::filesystem::path user_profile_dir() const { return cache_dir() / "user_profile"; }
		// user:// during Play in the editor: emptied at every start, so a session never sees the previous one's saves.
		std::filesystem::path play_profile_dir() const { return cache_dir() / "play_profile"; }
		// What the resource system mounts for writing besides the content roots, parents first.
		std::vector<std::filesystem::path> writable_dirs() const { return { cache_dir(), memory_dir(), user_profile_dir() }; }
		// <root>/.snk: the editor's own files (the MCP connection file with its token, the agent policy).
		std::filesystem::path snk_dir() const { return root / std::filesystem::path(k_snk_dir_name); }
		// <root>/code: the game module's sources, mounted as code:// when the folder exists (#450).
		std::filesystem::path code_dir() const { return root / std::filesystem::path(k_code_dir_name); }
		// <root>/build: where the game code is built (#160/#217). Not mounted; nothing writes there yet.
		std::filesystem::path build_dir() const { return root / std::filesystem::path(k_build_dir_name); }
		// Never content: no content root may be, contain or lie inside one of them.
		std::vector<std::filesystem::path> private_dirs() const { return { cache_dir(), snk_dir(), build_dir() }; }
	};

	struct open_result
	{
		std::optional<project_context> context;
		// Set exactly when context is empty: what to tell the user.
		std::string error;
	};

	// `path` is a project.snkproj file or the directory holding one.
	SNK_API open_result open_project(const std::filesystem::path& path);

	// What the resource system mounts for the project stays inside it. open_project checks it (without an engine base),
	// the resource system again right before it mounts. nullopt when everything holds:
	// - every content root is an existing directory inside the canonical project root, symlinks resolved;
	// - no content root lies inside another one, and none lies inside `engine_base` or contains it (empty: no base);
	// - no content root is, contains or lies inside a private dir (.cache, .snk, build): res:// would expose the agent
	//   token, write the test profile or serve build output. Compared with letter case folded and, where both exist, by
	//   fs::equivalent: on Windows and macOS .SNK is .snk;
	// - the path of each content root, symlinks resolved, passes content_root_error: a link to .git is refused too;
	// - each writable dir, and .snk, that exists is a real directory (not a symlink or junction) inside the project,
	//   and nothing at any depth in .cache is a link either (prj::link_below; past its limit the project is refused);
	// - code/, when it exists, is a real directory (not a symlink or junction) inside the project that no content root
	//   contains or lies inside, and that is none of the private dirs: code:// would otherwise write through a link or
	//   serve assets.
	SNK_API std::optional<std::string> verify_project_mounts(const project_context& project,
		const std::filesystem::path& engine_base = {});

	inline constexpr int k_exit_project_error = 2;
	inline constexpr int k_exit_engine_version = 3;

	// A project the host did not mount because of its engine version: an older one waits for the user's consent
	// (version_action::ask_open_or_quit), a newer one is refused. The host runs without a project meanwhile.
	struct held_project
	{
		enum class decision
		{
			none,
			continue_without_project,
			quit,
			// Not opened, and the run cannot go on without a project (`required`): ends like a run without --project.
			no_project,
		};

		std::filesystem::path manifest_file;
		std::string name;
		engine_version engine;
		version_action action = version_action::refuse;
		decision answer = decision::none;
		// project/required (--headless): going on without the project is a project error, not a choice.
		bool required = false;
	};

	// How the host exits after holding `project` back: 0 when the user chose to go on without it (a button, or Esc);
	// k_exit_project_error when the run needed it (no_project); otherwise k_exit_engine_version (the user quit, or the
	// window closed with the question unanswered).
	SNK_API int exit_code_for(const held_project& project);

	// True when the project must not be held for a question but refused at once: a non-interactive run
	// (--non-interactive, -n) never asks and takes the safe answer, and for a project of another engine version that
	// is not opening it. That covers an older project too (a headless run on one exits with 3); -n is never "yes to
	// everything": only --allow-engine-version-mismatch opens an older project, and then nothing is asked.
	SNK_API bool refuse_without_asking(version_action action, bool non_interactive);

	// The project given on the command line could not be opened; the host exits with k_exit_project_error.
	class SNK_API project_error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// The project was refused for its engine version without asking (refuse_without_asking); the host exits with
	// k_exit_engine_version before it opens a window.
	class SNK_API engine_version_error : public project_error
	{
	public:
		using project_error::project_error;
	};
}
