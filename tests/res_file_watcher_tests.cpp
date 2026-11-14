#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/resource/resolvers/res_file_watcher.h>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
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

BOOST_AUTO_TEST_CASE(KeepUnread_KeepsWhatWasKnownAtAndBelowAnUnreadPath)
{
	const fs::path root = fs::path("root");
	const fs::path other = fs::path("rooted");
	res::detail::watch_snapshot before;
	before.dirs = { root / "sub", other / "sub" };
	before.files[root / "a.txt"] = { {}, 1 };
	before.files[root / "sub" / "b.txt"] = { {}, 2 };
	before.files[other / "c.txt"] = { {}, 3 };

	// The scan of `root` stopped after a.txt, which it saw changed; `other` was read in full and lost c.txt.
	res::detail::watch_snapshot cut_short;
	cut_short.files[root / "a.txt"] = { {}, 10 };
	cut_short.dirs = { other / "sub" };
	res::detail::keep_unread(before, { root }, cut_short);
	BOOST_TEST(cut_short.files.size() == 2u);
	BOOST_TEST(cut_short.files.at(root / "a.txt").size == 1u);
	BOOST_TEST(cut_short.files.at(root / "sub" / "b.txt").size == 2u);
	BOOST_TEST(cut_short.dirs.size() == 2u);
	BOOST_TEST(res::detail::diff_snapshots(before, cut_short).size() == 1u);

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
		BOOST_TEST(change.kind == res::change_kind::deleted);
		BOOST_TEST((change.path == sub || change.path == sub / "inner.txt"));
	}

	watcher.stop();
	io.restart();
	io.run_for(50ms);
}

BOOST_AUTO_TEST_SUITE_END()
