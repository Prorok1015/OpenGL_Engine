#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/resource/resolvers/res_file_watcher.h>
#include <boost/asio/io_context.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// #939: one poll that throws must not be the watcher's last. The watcher polls on a real timer (500 ms), so these
// tests run its io_context until what they wait for has happened, for a few seconds at most.
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct watcher_fixture
{
	test_util::temp_dir dir{ "res_file_watcher" };
	boost::asio::io_context io;

	bool run_until(const std::function<bool()>& done)
	{
		const auto give_up = std::chrono::steady_clock::now() + 10s;
		while (!done() && std::chrono::steady_clock::now() < give_up) {
			io.restart();
			io.run_for(50ms);
		}
		return done();
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ResFileWatcherTests, watcher_fixture)

BOOST_AUTO_TEST_CASE(APollThatThrows_IsNotTheLast_PlainMode)
{
	std::vector<std::string> seen;
	bool thrown = false;
	res::file_watcher watcher(io, [&](fs::path changed) {
		seen.push_back(changed.filename().string());
		if (!thrown) {
			thrown = true;
			throw std::runtime_error("the watcher's user failed");
		}
	});
	watcher.add_path(dir.path());
	watcher.start();

	dir.write("first.txt", "1");
	BOOST_REQUIRE(run_until([&] { return thrown; }));

	dir.write("second.txt", "2");
	BOOST_TEST(run_until([&] { return seen.size() >= 2u; }));
	BOOST_REQUIRE(seen.size() >= 2u);
	BOOST_TEST(seen.back() == "second.txt");

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_CASE(APollThatThrows_IsNotTheLast_BatchMode)
{
	std::size_t batches = 0;
	bool thrown = false;
	res::file_watcher watcher(io);
	watcher.add_path(dir.path());
	res::watch_options options;
	options.report_changes = true;
	watcher.report_changes(options, [&](std::vector<res::raw_change>) {
		++batches;
		if (!thrown) {
			thrown = true;
			throw std::runtime_error("the watcher's user failed");
		}
	});
	watcher.start();

	dir.write("first.txt", "1");
	BOOST_REQUIRE(run_until([&] { return thrown; }));

	dir.write("second.txt", "2");
	BOOST_TEST(run_until([&] { return batches >= 2u; }));

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

// A watched folder that goes away and comes back, and a root that is a file: nothing to list is not an error.
BOOST_AUTO_TEST_CASE(ARootThatIsGoneOrNoFolder_IsPolledWithoutAnError)
{
	std::vector<std::string> seen;
	res::file_watcher watcher(io, [&](fs::path changed) { seen.push_back(changed.filename().string()); });
	const fs::path later = dir.path() / "later";
	dir.write("a_file", "x");
	watcher.add_path(later);
	watcher.add_path(dir.path() / "a_file");
	watcher.start();
	io.run_for(700ms);
	BOOST_TEST(seen.empty());

	fs::create_directories(later);
	dir.write("later/appeared.txt", "1");
	BOOST_TEST(run_until([&] { return !seen.empty(); }));
	BOOST_REQUIRE(!seen.empty());
	BOOST_TEST(seen.front() == "appeared.txt");

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_CASE(ACallbackThatThrows_DoesNotDropTheRestOfThePoll_PlainMode)
{
	std::vector<std::string> seen;
	res::file_watcher watcher(io, [&](fs::path changed) {
		seen.push_back(changed.filename().string());
		if (seen.size() == 1u) throw std::runtime_error("the watcher's user failed");
	});
	watcher.add_path(dir.path());
	watcher.start();

	// Both written before the io_context runs: one poll finds the two.
	dir.write("first.txt", "1");
	dir.write("second.txt", "2");
	BOOST_TEST(run_until([&] { return seen.size() >= 2u; }));
	BOOST_REQUIRE(seen.size() == 2u);
	BOOST_TEST(seen[0] != seen[1]);

	dir.write("third.txt", "3");
	BOOST_TEST(run_until([&] { return seen.size() >= 3u; }));
	BOOST_REQUIRE(seen.size() >= 3u);
	BOOST_TEST(seen.back() == "third.txt");

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_CASE(ACallbackThatThrows_DoesNotDropTheRestOfThePoll_BatchMode)
{
	std::vector<std::string> seen;
	std::vector<std::size_t> batch_sizes;
	res::file_watcher watcher(io, [&](fs::path changed) {
		seen.push_back(changed.filename().string());
		if (seen.size() == 1u) throw std::runtime_error("the watcher's user failed");
	});
	watcher.add_path(dir.path());
	res::watch_options options;
	options.report_changes = true;
	watcher.report_changes(options, [&](std::vector<res::raw_change> batch) { batch_sizes.push_back(batch.size()); });
	watcher.start();

	dir.write("first.txt", "1");
	dir.write("second.txt", "2");
	BOOST_TEST(run_until([&] { return !batch_sizes.empty(); }));
	BOOST_REQUIRE(batch_sizes.size() == 1u);
	BOOST_TEST(batch_sizes.front() == 2u);
	BOOST_TEST(seen.size() == 2u);

	dir.write("third.txt", "3");
	BOOST_TEST(run_until([&] { return batch_sizes.size() >= 2u; }));
	BOOST_TEST(seen.size() >= 3u);

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_CASE(AStopFromACallback_DoesNotArmTheTimerAgain)
{
	std::size_t calls = 0;
	res::file_watcher* self = nullptr;
	res::file_watcher watcher(io, [&](fs::path) {
		++calls;
		self->stop();
	});
	self = &watcher;
	watcher.add_path(dir.path());
	watcher.start();

	dir.write("first.txt", "1");
	BOOST_REQUIRE(run_until([&] { return calls == 1u; }));

	// More than two poll periods: a timer armed again after the stop would fire here.
	dir.write("second.txt", "2");
	io.restart();
	BOOST_TEST(io.run_for(1200ms) == 0u);
	BOOST_TEST(calls == 1u);
}

BOOST_AUTO_TEST_CASE(KeepUnread_HoldsBackOnlyDeletionsBelowAnUnreadPath)
{
	const fs::path root = fs::path("root");
	const fs::path other = fs::path("rooted");
	res::detail::watch_snapshot before;
	before.dirs = { root / "sub", other / "sub" };
	before.files[root / "a.txt"] = { {}, 1 };
	before.files[root / "sub" / "b.txt"] = { {}, 2 };
	before.files[other / "c.txt"] = { {}, 3 };

	// The scan of `root` stopped after a.txt, which it saw changed, and a new file; `other` was read in full and
	// lost c.txt.
	res::detail::watch_snapshot cut_short;
	cut_short.files[root / "a.txt"] = { {}, 10 };
	cut_short.files[root / "new.txt"] = { {}, 4 };
	cut_short.dirs = { other / "sub" };
	res::detail::keep_unread(before, { root }, cut_short);
	BOOST_TEST(cut_short.files.size() == 3u);
	BOOST_TEST(cut_short.files.at(root / "a.txt").size == 10u);
	BOOST_TEST(cut_short.files.at(root / "new.txt").size == 4u);
	BOOST_TEST(cut_short.files.at(root / "sub" / "b.txt").size == 2u);
	BOOST_TEST(cut_short.dirs.size() == 2u);
	// c.txt deleted, new.txt created, a.txt modified; nothing about sub/b.txt, which the scan did not reach.
	const auto changes = res::detail::diff_snapshots(before, cut_short);
	BOOST_REQUIRE(changes.size() == 3u);
	for (const auto& change : changes) {
		BOOST_TEST((change.path != root / "sub" / "b.txt"));
		BOOST_TEST((change.path != root / "sub"));
	}

	// One file that could not be asked about keeps its old state; one that was never known stays unknown.
	res::detail::watch_snapshot one_unread;
	one_unread.dirs = before.dirs;
	one_unread.files[root / "a.txt"] = { {}, 1 };
	one_unread.files[other / "c.txt"] = { {}, 3 };
	res::detail::keep_unread(before, { root / "sub" / "b.txt", root / "sub" / "new.txt" }, one_unread);
	BOOST_TEST(res::detail::diff_snapshots(before, one_unread).empty());

	BOOST_TEST(res::detail::is_at_or_below(root / "sub", fs::path("root/")));
	BOOST_TEST(!res::detail::is_at_or_below(other, root));
}

BOOST_AUTO_TEST_CASE(AdoptFirstRead_TakesWhatTheFirstScanMissedAsKnown)
{
	const fs::path root = fs::path("root");
	res::detail::watch_snapshot known;
	known.files[root / "a.txt"] = { {}, 1 };

	res::detail::watch_snapshot found;
	found.dirs = { root / "sub" };
	found.files[root / "a.txt"] = { {}, 1 };
	found.files[root / "sub" / "b.txt"] = { {}, 2 };
	found.files[root / "new.txt"] = { {}, 3 };

	// `sub` was unread at the start and is read now: it becomes known, new.txt beside it is still a creation.
	const auto still = res::detail::adopt_first_read({ root / "sub" }, {}, found, known);
	BOOST_TEST(still.empty());
	BOOST_TEST(known.dirs.contains(root / "sub"));
	BOOST_TEST(known.files.contains(root / "sub" / "b.txt"));
	const auto changes = res::detail::diff_snapshots(known, found);
	BOOST_REQUIRE(changes.size() == 1u);
	BOOST_TEST((changes.front().path == root / "new.txt"));

	// Still unread, as a whole or in a part: that is what waits for its first read.
	res::detail::watch_snapshot scratch;
	const auto whole = res::detail::adopt_first_read({ root / "sub" }, { root }, found, scratch);
	BOOST_TEST((whole == std::set<fs::path>{ root / "sub" }));
	const auto part = res::detail::adopt_first_read({ root }, { root / "sub" }, found, scratch);
	BOOST_TEST((part == std::set<fs::path>{ root / "sub" }));
}

// The scan of the root fails on every poll: what it did find is reported, only the deletion waits.
BOOST_AUTO_TEST_CASE(ARootUnreadOnEveryPoll_StillReportsACreatedFile_BatchMode)
{
	dir.write("old.txt", "x");
	const fs::path root = dir.path();

	std::vector<res::raw_change> reported;
	res::file_watcher watcher(io);
	watcher.add_path(root);
	res::watch_options options;
	options.report_changes = true;
	watcher.report_changes(options, [&](std::vector<res::raw_change> batch) {
		reported.insert(reported.end(), batch.begin(), batch.end());
	});
	watcher.start();
	// After the start: a root unread by the first scan is a baseline still to be read, not a change.
	watcher.fail_reads_for_test([&](const fs::path& path) { return path == root; });

	std::error_code ec;
	fs::remove(root / "old.txt", ec);
	dir.write("new.txt", "12");
	BOOST_TEST(run_until([&] { return !reported.empty(); }));
	io.restart();
	io.run_for(700ms);
	BOOST_REQUIRE(reported.size() == 1u);
	BOOST_TEST((reported.front().path == root / "new.txt"));
	BOOST_TEST((reported.front().kind == res::change_kind::created));

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_CASE(WhatTheFirstScanCouldNotRead_IsNotReportedAsCreated_BatchMode)
{
	dir.write("late.txt", "x");
	const fs::path root = dir.path();

	bool fail = true;
	std::vector<res::raw_change> reported;
	res::file_watcher watcher(io);
	watcher.add_path(root);
	res::watch_options options;
	options.report_changes = true;
	watcher.report_changes(options, [&](std::vector<res::raw_change> batch) {
		reported.insert(reported.end(), batch.begin(), batch.end());
	});
	watcher.fail_reads_for_test([&](const fs::path& path) { return fail && path == root / "late.txt"; });
	watcher.start();
	fail = false;

	dir.write("new.txt", "12");
	BOOST_TEST(run_until([&] { return !reported.empty(); }));
	BOOST_REQUIRE(reported.size() == 1u);
	BOOST_TEST((reported.front().path == root / "new.txt"));

	// Known since its first read: a later change of it is reported as any other.
	dir.write("late.txt", "a longer text");
	BOOST_TEST(run_until([&] { return reported.size() >= 2u; }));
	BOOST_REQUIRE(reported.size() >= 2u);
	BOOST_TEST((reported.back().path == root / "late.txt"));
	BOOST_TEST((reported.back().kind == res::change_kind::modified));

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

// A folder removed while it is listed cuts the scan short where the iterator reports that (and is only a deletion
// where it does not): either way the files the scan had not reached are not reported as deleted.
BOOST_AUTO_TEST_CASE(AFolderRemovedDuringAScan_DoesNotReportItsNeighbours_BatchMode)
{
	for (int i = 0; i < 16; ++i) {
		dir.write("file_" + std::to_string(i) + ".txt", "x");
	}
	dir.write("sub/inner.txt", "x");
	const fs::path sub = dir.path() / "sub";

	bool remove_on_scan = false;
	std::vector<res::raw_change> reported;
	res::file_watcher watcher(io);
	watcher.add_path(dir.path());
	res::watch_options options;
	options.report_changes = true;
	options.ignore_dir = [&](std::string_view name) {
		if (remove_on_scan && name == "sub") {
			remove_on_scan = false;
			std::error_code ec;
			fs::remove_all(sub, ec);
		}
		return false;
	};
	watcher.report_changes(options, [&](std::vector<res::raw_change> batch) {
		reported.insert(reported.end(), batch.begin(), batch.end());
	});
	watcher.start();
	remove_on_scan = true;

	const auto sub_is_gone = [&] {
		for (const auto& change : reported) {
			if (change.is_directory && change.kind == res::change_kind::deleted && change.path == sub) return true;
		}
		return false;
	};
	BOOST_TEST(run_until(sub_is_gone));
	for (const auto& change : reported) {
		BOOST_TEST((change.kind == res::change_kind::deleted));
		BOOST_TEST((change.path == sub || change.path == sub / "inner.txt"));
	}

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

// #975: a file whose stamp was racy when a poll recorded it is looked at again once the stamp has aged: modified only
// when its content differs from then, or cannot be compared (larger than the limit, unreadable); never an own write.
BOOST_AUTO_TEST_CASE(RecheckRacy_ReportsAnAgedFileOnlyWhenItsContentChanged)
{
	const fs::file_time_type stamp = fs::file_time_type::clock::now();
	const fs::file_time_type racy = stamp + 1s;
	const fs::file_time_type aged = stamp + res::k_stamp_granularity + 1s;
	const fs::path same = "root/same.desc";
	const fs::path rewritten = "root/rewritten.desc";
	const fs::path large = "root/large.bin";
	const fs::path unreadable = "root/unreadable.desc";
	const fs::path own = "root/own.desc";
	const fs::path grown = "root/grown.desc";
	std::map<fs::path, std::optional<std::string>> content{ { same, "a" }, { rewritten, "a" }, { unreadable, "a" },
		{ own, "a" }, { grown, "a" } };
	int large_reads = 0;
	const res::detail::content_hash_source hash_of = [&](const fs::path& path) -> std::optional<std::string> {
		if (path == large) ++large_reads;
		return content.contains(path) ? content.at(path) : std::nullopt;
	};
	const auto recheck = [&](const std::map<fs::path, res::detail::racy_file>& before,
		res::detail::watch_snapshot& after, fs::file_time_type now) {
		res::detail::racy_budget budget;
		return res::detail::recheck_racy(before, after, now, hash_of, {}, budget);
	};
	res::detail::watch_snapshot first;
	for (const auto& path : { same, rewritten, unreadable, own, grown }) first.files[path] = { stamp, 10 };
	first.files[large] = { stamp, res::detail::k_racy_hash_limit + 1 };
	BOOST_TEST(recheck({}, first, racy).empty());
	BOOST_TEST(first.racy.size() == 6u);
	BOOST_TEST(large_reads == 0);
	BOOST_TEST(!first.racy.at(large).hash.has_value());
	first.racy.at(own).own = true;

	// Still racy: nothing yet, whatever the content does.
	content[rewritten] = "b";
	res::detail::watch_snapshot second;
	second.files = first.files;
	BOOST_TEST(recheck(first.racy, second, racy).empty());
	BOOST_TEST(second.racy.size() == 6u);

	content[unreadable] = std::nullopt;
	content[own] = "b";
	res::detail::watch_snapshot third;
	third.files = second.files;
	third.files[grown] = { stamp, 11 };
	const std::vector<res::raw_change> changed = recheck(second.racy, third, aged);
	std::set<fs::path> reported;
	for (const auto& change : changed) {
		BOOST_TEST((change.kind == res::change_kind::modified));
		reported.insert(change.path);
	}
	// grown: diff_snapshots reports it by its size.
	BOOST_TEST((reported == std::set<fs::path>{ rewritten, large, unreadable }));
	BOOST_TEST(third.racy.empty());
	BOOST_TEST(large_reads == 0);

	// Aged once: the next poll reports nothing more.
	res::detail::watch_snapshot fourth;
	fourth.files = third.files;
	BOOST_TEST(recheck(third.racy, fourth, aged + 1s).empty());
}

// #975: one poll reads at most its budget; the rest waits for the next polls, and what ages before it was hashed is
// reported. What lies below an unread path is neither read nor reported.
BOOST_AUTO_TEST_CASE(RecheckRacy_ReadsWithinItsBudget_AndLeavesUnreadPathsAlone)
{
	const fs::file_time_type stamp = fs::file_time_type::clock::now();
	const fs::file_time_type racy = stamp + 1s;
	const fs::file_time_type aged = stamp + res::k_stamp_granularity + 1s;
	int reads = 0;
	const res::detail::content_hash_source hash_of = [&reads](const fs::path&) -> std::optional<std::string> {
		++reads;
		return std::string("same");
	};
	res::detail::watch_snapshot first;
	for (int i = 0; i < 3; ++i) first.files[std::format("root/{}.desc", i)] = { stamp, 10 };
	first.files["root/locked/x.desc"] = { stamp, 10 };
	res::detail::racy_budget two{ 2, 1024 };
	BOOST_TEST(res::detail::recheck_racy({}, first, racy, hash_of, { "root/locked" }, two).empty());
	BOOST_TEST(reads == 2);
	BOOST_TEST(first.racy.size() == 3u);
	BOOST_TEST(first.racy.at("root/2.desc").waiting);

	// The next poll, still racy, hashes the one that waited.
	res::detail::watch_snapshot second;
	second.files = first.files;
	res::detail::racy_budget more;
	BOOST_TEST(res::detail::recheck_racy(first.racy, second, racy, hash_of, {}, more).empty());
	BOOST_TEST(reads == 4);
	BOOST_TEST(!second.racy.at("root/2.desc").waiting);
	BOOST_TEST(second.racy.contains("root/locked/x.desc"));

	// Aged with the folder unread again: kept as it was, not reported.
	res::detail::watch_snapshot third;
	third.files = second.files;
	res::detail::racy_budget none{ 0, 0 };
	BOOST_TEST(res::detail::recheck_racy(second.racy, third, aged, hash_of, { "root/locked" }, none).empty());
	BOOST_TEST(third.racy.contains("root/locked/x.desc"));
	// No budget left to compare the others: they wait for the next poll, still remembered.
	BOOST_TEST(third.racy.size() == 4u);

	// Aged and never hashed: reported.
	res::detail::watch_snapshot never;
	never.files["root/new.desc"] = { stamp, 10 };
	BOOST_TEST(res::detail::recheck_racy({}, never, racy, hash_of, {}, none).empty());
	res::detail::watch_snapshot later;
	later.files = never.files;
	const auto reported = res::detail::recheck_racy(never.racy, later, aged, hash_of, {}, more);
	BOOST_REQUIRE(reported.size() == 1u);
	BOOST_TEST(reported.front().path == fs::path("root/new.desc"));
}

// #975: an outside rewrite of the same size whose stamp is set back to what it was (cp -p, a script, git) is reported
// once the stamp has aged; the watcher's own write is not.
BOOST_AUTO_TEST_CASE(ARewriteInTheSameTick_IsReportedOnceTheStampHasAged_AnOwnWriteIsNot)
{
	res::file_watcher watcher(io);
	watcher.add_path(dir.path());
	res::watch_options options;
	options.report_changes = true;
	std::vector<res::raw_change> seen;
	watcher.report_changes(options, [&](std::vector<res::raw_change> batch) {
		seen.insert(seen.end(), batch.begin(), batch.end());
	});
	const fs::path file = dir.write("level.desc", "{\"ref\":\"aaaa\"}");
	const fs::path own = dir.write("own.desc", "{\"ref\":\"aaaa\"}");
	const fs::file_time_type stamp = fs::last_write_time(file);
	fs::last_write_time(own, stamp);
	fs::file_time_type now = stamp + 1s;
	watcher.set_clock_for_test([&now] { return now; });
	watcher.start();
	const auto modified = [&](const fs::path& path) {
		return std::ranges::any_of(seen, [&](const res::raw_change& change) {
			return change.path == path && change.kind == res::change_kind::modified;
		});
	};

	dir.write("level.desc", "{\"ref\":\"bbbb\"}");
	fs::last_write_time(file, stamp);
	dir.write("own.desc", "{\"ref\":\"bbbb\"}");
	fs::last_write_time(own, stamp);
	watcher.mark_written(own);
	watcher.poll_for_test();
	BOOST_TEST(seen.empty());

	now = stamp + res::k_stamp_granularity + 1s;
	watcher.poll_for_test();
	BOOST_TEST(modified(file));
	BOOST_TEST(!modified(own));

	seen.clear();
	watcher.poll_for_test();
	BOOST_TEST(seen.empty());
	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_SUITE_END()
