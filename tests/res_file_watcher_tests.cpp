#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/resource/resolvers/res_file_watcher.h>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
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

BOOST_AUTO_TEST_SUITE_END()
