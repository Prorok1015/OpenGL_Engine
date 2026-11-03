#include <boost/test/unit_test.hpp>
#include "edt_repository_watcher.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// The repository watcher (#485): which file of a repository is a change of which kind, its poll interval, the refs
// budget and the editor's own git jobs. A fake file system and a fake clock: nothing sleeps.
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using watcher_t = edt::vcs::repository_watcher;

struct fake_entry
{
	edt::vcs::file_stamp stamp{};
	bool is_dir = false;
};

struct watcher_fixture
{
	std::map<fs::path, fake_entry> entries;
	std::int64_t next_mtime = 1;
	watcher_t::clock::time_point now{};
	// Each folder listing takes this long (by the fake clock).
	watcher_t::clock::duration listing_takes{};
	bool own_running = false;
	std::size_t stats = 0;
	std::unique_ptr<watcher_t> watcher;

	const fs::path repo{ "/repo" };
	const fs::path git_dir = repo / ".git";

	watcher_fixture()
	{
		write(git_dir / "HEAD", 21);
		write(git_dir / "index", 100);
		write(git_dir / "packed-refs", 50);
		write(git_dir / "refs" / "heads" / "main", 41);
		make_dir(git_dir / "refs" / "tags");
		make_dir(git_dir / "objects" / "ab");
		write(repo / "CMakeLists.txt", 10);
		write(repo / ".gitignore", 10);
		write(repo / "src" / "a.cpp", 10);
	}

	void touch_parent(const fs::path& path)
	{
		const fs::path parent = path.parent_path();
		if (parent == path) return;
		make_dir(parent);
		entries[parent].stamp.mtime = next_mtime++;
	}

	void make_dir(const fs::path& path)
	{
		if (entries.contains(path) || path == path.parent_path()) return;
		touch_parent(path);
		entries[path] = fake_entry{ { next_mtime++, 0 }, true };
	}

	// A new file changes its folder's write time, as on a real file system; an existing one only its own.
	void write(const fs::path& path, std::uint64_t size)
	{
		if (!entries.contains(path)) touch_parent(path);
		entries[path] = fake_entry{ { next_mtime++, size }, false };
	}

	void remove(const fs::path& path)
	{
		entries.erase(path);
		touch_parent(path);
	}

	watcher_t::ports ports()
	{
		watcher_t::ports out;
		out.now = [this] { return now; };
		out.stat_file = [this](const fs::path& path) -> std::optional<edt::vcs::file_stamp> {
			++stats;
			const auto found = entries.find(path);
			if (found == entries.end()) return std::nullopt;
			return found->second.stamp;
		};
		out.list_dir = [this](const fs::path& path) {
			now += listing_takes;
			std::vector<edt::vcs::dir_entry> listed;
			for (const auto& [name, entry] : entries) {
				if (name.parent_path() == path && name != path) listed.push_back(edt::vcs::dir_entry{ name.filename(), entry.is_dir });
			}
			return listed;
		};
		out.own_jobs_running = [this] { return own_running; };
		return out;
	}

	void arm(const fs::path& git, const fs::path& common, const fs::path& root, const fs::path& project)
	{
		watcher = std::make_unique<watcher_t>(ports());
		watcher->set_paths(git, common, root, project);
	}

	void arm() { arm(git_dir, git_dir, repo, repo); }

	edt::vcs::repository_changes poll()
	{
		now += watcher_t::k_poll_interval;
		return watcher->tick();
	}

	bool only_refs()
	{
		const auto changes = poll();
		return changes.refs && !changes.index;
	}

	bool only_index()
	{
		const auto changes = poll();
		return changes.index && !changes.refs;
	}

	bool nothing() { return !poll().any(); }
};

}

BOOST_FIXTURE_TEST_SUITE(EdtRepositoryWatcher, watcher_fixture)

BOOST_AUTO_TEST_CASE(NothingChanged_NothingIsReported)
{
	arm();
	BOOST_TEST(watcher->armed());
	BOOST_TEST(nothing());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(HeadRefsPackedRefsAndFetchHead_AreRefsChanges_OncePerPoll)
{
	arm();
	write(git_dir / "HEAD", 25);
	BOOST_TEST(only_refs());
	BOOST_TEST(nothing());

	write(git_dir / "refs" / "heads" / "main", 41);
	BOOST_TEST(only_refs());
	write(git_dir / "refs" / "heads" / "feature", 41);
	BOOST_TEST(only_refs());
	write(git_dir / "refs" / "tags" / "v1", 41);
	BOOST_TEST(only_refs());
	remove(git_dir / "refs" / "heads" / "feature");
	BOOST_TEST(only_refs());
	write(git_dir / "packed-refs", 90);
	BOOST_TEST(only_refs());
	write(git_dir / "FETCH_HEAD", 70);
	BOOST_TEST(only_refs());
	write(git_dir / "ORIG_HEAD", 41);
	write(git_dir / "MERGE_HEAD", 41);
	BOOST_TEST(only_refs());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(TheIndexAndTopLevelFiles_AreIndexChanges)
{
	arm(git_dir, git_dir, repo, repo / "game");
	write(repo / "game" / "project.snkproj", 5);
	BOOST_TEST(only_index());
	write(git_dir / "index", 120);
	BOOST_TEST(only_index());
	write(repo / "CMakeLists.txt", 11);
	BOOST_TEST(only_index());
	write(repo / "README.md", 3);
	BOOST_TEST(only_index());
	remove(repo / ".gitignore");
	BOOST_TEST(only_index());
	write(repo / "game" / "project.snkproj", 6);
	BOOST_TEST(only_index());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(SubfoldersAndObjects_AreNotWatched)
{
	arm();
	write(repo / "src" / "a.cpp", 11);
	write(repo / "src" / "b.cpp", 11);
	write(git_dir / "objects" / "ab" / "cdef", 200);
	write(git_dir / "logs" / "HEAD", 200);
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(ItPolls_NoMoreOftenThanEvery500Ms)
{
	arm();
	write(git_dir / "HEAD", 25);
	const std::size_t stats_before = stats;
	now += watcher_t::k_poll_interval - 1ms;
	BOOST_TEST(!watcher->tick().any());
	BOOST_TEST(stats == stats_before);
	now += 1ms;
	BOOST_TEST(watcher->tick().refs);
	BOOST_TEST(!watcher->tick().any());
}

BOOST_AUTO_TEST_CASE(TooManyRefs_WatchesOnlyTheirFolders_AndStillSeesANewBranchFolder)
{
	for (std::size_t i = 0; i <= watcher_t::k_refs_budget; ++i) {
		write(git_dir / "refs" / "remotes" / "origin" / std::format("b{}", i), 41);
	}
	arm();
	BOOST_TEST(watcher->refs_folders_only());
	BOOST_TEST(nothing());
	write(git_dir / "refs" / "heads" / "feature" / "x", 41);
	BOOST_TEST(only_refs());
	// The new folder is watched too.
	write(git_dir / "refs" / "heads" / "feature" / "y", 41);
	BOOST_TEST(only_refs());
	write(git_dir / "HEAD", 30);
	BOOST_TEST(only_refs());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(ASlowRefsWalk_WatchesOnlyTheirFolders)
{
	listing_takes = 3ms;
	arm();
	BOOST_TEST(watcher->refs_folders_only());
	BOOST_TEST(nothing());
	write(git_dir / "refs" / "heads" / "topic", 41);
	BOOST_TEST(only_refs());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(AFastWalkOfFewRefs_WatchesEveryFile)
{
	listing_takes = 100us;
	arm();
	BOOST_TEST(!watcher->refs_folders_only());
}

BOOST_AUTO_TEST_CASE(AnIndexChangeOfAnOwnJob_IsNotReported_DuringItNorAfterIt)
{
	arm();
	watcher->begin_own_job();
	own_running = true;
	write(git_dir / "index", 130);
	write(repo / "CMakeLists.txt", 12);
	BOOST_TEST(nothing());
	BOOST_TEST(nothing());
	own_running = false;
	BOOST_TEST(nothing());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(ARefsChangeDuringAnOwnJob_IsReported)
{
	arm();
	watcher->begin_own_job();
	own_running = true;
	write(git_dir / "index", 130);
	write(git_dir / "refs" / "heads" / "main", 41);
	BOOST_TEST(only_refs());
	own_running = false;
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(AShortOwnJobBetweenTwoTicks_StillHidesItsIndexChange)
{
	arm();
	// Started and ended before the next tick: the running count is back to 0 when tick looks.
	watcher->begin_own_job();
	write(git_dir / "index", 130);
	BOOST_TEST(nothing());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(AnIndexChangeBeforeAnOwnJob_IsStillReported)
{
	arm();
	write(git_dir / "index", 130);
	watcher->begin_own_job();
	own_running = true;
	BOOST_TEST(only_index());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(AJobSeenOnlyThroughTheRunningCount_StillHidesItsIndexChange)
{
	arm();
	own_running = true;
	BOOST_TEST(nothing());
	write(git_dir / "index", 130);
	BOOST_TEST(nothing());
	own_running = false;
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(ALinkedWorktree_TakesHeadAndIndexFromItsOwnGitDir_AndRefsFromTheCommonOne)
{
	const fs::path tree = "/tree";
	const fs::path own = git_dir / "worktrees" / "tree";
	write(own / "HEAD", 30);
	write(own / "index", 100);
	write(tree / "CMakeLists.txt", 10);
	arm(own, git_dir, tree, tree);

	write(git_dir / "HEAD", 25);
	write(git_dir / "index", 140);
	write(repo / "CMakeLists.txt", 20);
	BOOST_TEST(nothing());
	write(own / "HEAD", 31);
	BOOST_TEST(only_refs());
	write(git_dir / "refs" / "heads" / "other", 41);
	BOOST_TEST(only_refs());
	write(git_dir / "packed-refs", 80);
	BOOST_TEST(only_refs());
	write(own / "index", 101);
	BOOST_TEST(only_index());
	write(tree / "CMakeLists.txt", 11);
	BOOST_TEST(only_index());
	BOOST_TEST(nothing());
}

BOOST_AUTO_TEST_CASE(WithoutAGitDir_ItIsOff_AndReArmingReportsNothing)
{
	arm();
	watcher->set_paths({}, {}, {}, {});
	BOOST_TEST(!watcher->armed());
	write(git_dir / "HEAD", 25);
	BOOST_TEST(nothing());
	watcher->set_paths(git_dir, git_dir, repo, repo);
	BOOST_TEST(watcher->armed());
	BOOST_TEST(nothing());
	write(git_dir / "HEAD", 26);
	BOOST_TEST(only_refs());
}

BOOST_AUTO_TEST_CASE(TheDefaultPorts_ReadARealFolder_WithoutThrowing)
{
	const fs::path missing = fs::temp_directory_path() / "snk_edt_repository_watcher_missing" / "nothing";
	BOOST_TEST(!watcher_t::default_stat(missing).has_value());
	BOOST_TEST(watcher_t::default_list_dir(missing).empty());
	const auto folder = watcher_t::default_stat(fs::temp_directory_path());
	BOOST_TEST(folder.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
