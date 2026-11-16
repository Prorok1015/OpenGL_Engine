#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_desc_binary_hash_cache.h>
#include <snk/resource/res_system.h>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// The cache of checked pairs (EPIC-79 section 7, #1126): an unchanged pair is not hashed again, any change is; several
// roots and a staged file always hash; the file cache trusts nothing it cannot read exactly.

namespace {

namespace fs = std::filesystem;

const std::vector<float> k_positions{ 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
const std::vector<std::uint32_t> k_indices{ 0, 1, 1 };

std::vector<std::byte> encoded(std::vector<res::binary_range>& ranges)
{
	const std::array chunks{ res::binary_chunk{ std::as_bytes(std::span(k_positions)), "f32x3" },
		res::binary_chunk{ std::as_bytes(std::span(k_indices)), "u32" } };
	std::string error;
	auto file = res::desc_binary::encode(chunks, ranges, error);
	BOOST_REQUIRE_MESSAGE(file.has_value(), error);
	return *file;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_text(const fs::path& path, const std::string& text)
{
	write_file(path, std::as_bytes(std::span(text)));
}

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::string desc_text(std::span<const res::binary_range> ranges)
{
	boost::json::object desc;
	desc["vertices"] = res::to_json(ranges[0]);
	desc["indices"] = res::to_json(ranges[1]);
	return boost::json::serialize(desc);
}

// Older than the 2 s a fresh write time is not trusted for.
void backdate(const fs::path& path, std::chrono::seconds age = std::chrono::hours(1))
{
	fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

std::uint64_t hashed()
{
	return res::detail::desc_binary_hashed_ranges().load();
}

struct memory_cache final : res::desc_binary_hash_cache
{
	std::vector<res::desc_binary_pair_key> keys;
	std::size_t remembered = 0;

	bool verified(const res::desc_binary_pair_key& key) const override
	{
		return std::find(keys.begin(), keys.end(), key) != keys.end();
	}
	void remember(const res::desc_binary_pair_key& key) override
	{
		++remembered;
		keys.push_back(key);
	}
};

// res:// over `top`, or over `top` and `lower`; a pair written in `top`.
struct pair_fixture
{
	test_util::temp_dir dir{ "res_desc_binary_hash_cache" };
	fs::path top = dir.path() / "top";
	fs::path lower = dir.path() / "lower";
	fs::path cache_dir = dir.path() / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	const res::tag desc{ "res://a/robot.geom.desc" };
	std::vector<res::binary_range> ranges;
	std::vector<std::byte> file;
	std::shared_ptr<memory_cache> cache = std::make_shared<memory_cache>();

	pair_fixture()
	{
		fs::create_directories(top / "a");
		fs::create_directories(lower / "a");
		fs::create_directories(cache_dir / "memory");
		fs::create_directories(cache_dir / "user_profile");
		file = encoded(ranges);
		write_file(bin(), file);
		write_text(desc_file(), desc_text(ranges));
		backdate(bin());
		backdate(desc_file());
	}

	res::mount_table single_root() const
	{
		return res::resource_system::project_mount_table({}, { top }, cache_dir, cache_dir / "user_profile");
	}
	res::mount_table two_roots() const
	{
		return res::resource_system::project_mount_table({}, { top, lower }, cache_dir, cache_dir / "user_profile");
	}

	fs::path bin() const { return top / "a" / "robot.geom.bin"; }
	fs::path desc_file() const { return top / "a" / "robot.geom.desc"; }
	fs::path staged() const { return top / "a" / "robot.geom.bin.new"; }

	res::desc_binary_data read(res::resource_system& sys) const { return res::desc_binary::read(sys, desc, ranges, true); }

	// A read that hashed (or not) the ranges asked for.
	bool read_hashes(res::resource_system& sys) const
	{
		const std::uint64_t before = hashed();
		const auto data = read(sys);
		BOOST_REQUIRE_MESSAGE(data, data.message);
		return hashed() != before;
	}
};

res::desc_binary_pair_key some_key(std::string binary)
{
	return { std::move(binary), { 10, 1'700'000'000'000'000'000 }, { 20, 1'700'000'000'000'000'001 },
		std::string(64, 'a') };
}

}

BOOST_AUTO_TEST_SUITE(ResDescBinaryHashCacheTests)

BOOST_AUTO_TEST_CASE(AMovedDataKeepsItsRanges)
{
	std::vector<res::binary_range> ranges;
	auto file = encoded(ranges);
	const std::vector<std::byte> expected(file.begin() + static_cast<std::ptrdiff_t>(ranges[1].offset),
		file.begin() + static_cast<std::ptrdiff_t>(ranges[1].offset + ranges[1].size));
	res::desc_binary_data data = res::desc_binary::parse("f.bin", std::move(file), ranges, true);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	const res::desc_binary_data moved = std::move(data);
	BOOST_REQUIRE(moved.ranges.size() == 2u);
	BOOST_TEST(std::equal(moved.ranges[1].begin(), moved.ranges[1].end(), expected.begin(), expected.end()));
	BOOST_TEST((moved.ranges[0].data() >= moved.file.data() && moved.ranges[1].data() < moved.file.data() + moved.file.size()));
}

BOOST_FIXTURE_TEST_CASE(ASecondReadOfAnUnchangedPairIsNotHashed, pair_fixture)
{
	res::resource_system sys(tasks, single_root());
	sys.set_desc_binary_hash_cache(cache);
	const std::uint64_t before = hashed();
	const auto first = read(sys);
	BOOST_REQUIRE_MESSAGE(first, first.message);
	BOOST_TEST(hashed() == before + ranges.size());
	BOOST_TEST(cache->remembered == 1u);

	const std::uint64_t after_first = hashed();
	const auto second = read(sys);
	BOOST_REQUIRE_MESSAGE(second, second.message);
	BOOST_TEST(hashed() == after_first);
	BOOST_TEST(cache->remembered == 1u);
	BOOST_REQUIRE(second.ranges.size() == first.ranges.size());
	for (std::size_t i = 0; i < first.ranges.size(); ++i) {
		BOOST_TEST(std::equal(first.ranges[i].begin(), first.ranges[i].end(), second.ranges[i].begin(), second.ranges[i].end()));
	}
}

BOOST_FIXTURE_TEST_CASE(AChangeOfEitherFileHashesAgain, pair_fixture)
{
	res::resource_system sys(tasks, single_root());
	sys.set_desc_binary_hash_cache(cache);
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(!read_hashes(sys));

	// The binary file's write time.
	backdate(bin(), std::chrono::hours(2));
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(!read_hashes(sys));

	// Its size: one byte past the last range.
	auto longer = file;
	longer.push_back(std::byte{ 0 });
	write_file(bin(), longer);
	backdate(bin(), std::chrono::hours(3));
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(!read_hashes(sys));

	// The desc's write time.
	backdate(desc_file(), std::chrono::hours(2));
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(!read_hashes(sys));
}

BOOST_FIXTURE_TEST_CASE(OtherRangesAskedForAreHashed, pair_fixture)
{
	res::resource_system sys(tasks, single_root());
	sys.set_desc_binary_hash_cache(cache);
	BOOST_TEST(read_hashes(sys));

	auto other = ranges;
	other[0].sha256 = std::string(64, '0');
	const std::uint64_t before = hashed();
	const auto refused = res::desc_binary::read(sys, desc, other, true);
	BOOST_TEST((refused.error == res::desc_binary_error::hash));
	BOOST_TEST(hashed() > before);
}

BOOST_FIXTURE_TEST_CASE(APairWrittenWithinTwoSecondsIsNotRemembered, pair_fixture)
{
	res::resource_system sys(tasks, single_root());
	sys.set_desc_binary_hash_cache(cache);
	write_file(bin(), file);
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(cache->remembered == 0u);

	backdate(bin());
	write_text(desc_file(), desc_text(ranges));
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(cache->remembered == 0u);
}

BOOST_FIXTURE_TEST_CASE(SeveralRootsAndAStagedFileHashEveryTime, pair_fixture)
{
	{
		res::resource_system single(tasks, single_root());
		single.set_desc_binary_hash_cache(cache);
		BOOST_TEST(read_hashes(single));
		BOOST_TEST(!read_hashes(single));
	}
	{
		res::resource_system layered(tasks, two_roots());
		layered.set_desc_binary_hash_cache(cache);
		BOOST_TEST(read_hashes(layered));
		BOOST_TEST(read_hashes(layered));
	}
	res::resource_system single(tasks, single_root());
	single.set_desc_binary_hash_cache(cache);
	write_file(staged(), file);
	backdate(staged());
	BOOST_TEST(read_hashes(single));
	BOOST_TEST(read_hashes(single));
}

BOOST_FIXTURE_TEST_CASE(TheDebugSwitchSkipsHashingOnOneRootOnly, pair_fixture)
{
	{
		res::resource_system single(tasks, single_root());
		single.set_verify_desc_binary_hash(false);
		BOOST_TEST(!single.verify_desc_binary_hash());
		BOOST_TEST(!read_hashes(single));
	}
	res::resource_system layered(tasks, two_roots());
	layered.set_verify_desc_binary_hash(false);
	BOOST_TEST(read_hashes(layered));
}

BOOST_FIXTURE_TEST_CASE(TheCacheAndTheSwitchAreKeptByTheRoot, pair_fixture)
{
	res::resource_system sys(tasks, single_root());
	res::resource_system layer(sys);
	layer.set_desc_binary_hash_cache(cache);
	layer.set_verify_desc_binary_hash(false);
	BOOST_TEST((sys.desc_binary_hash_cache_ptr() == cache));
	BOOST_TEST(!sys.verify_desc_binary_hash());
}

// The stamps match, so the hashes are skipped; the header is checked all the same.
BOOST_FIXTURE_TEST_CASE(AHitStillChecksTheStructureOfTheFile, pair_fixture)
{
	res::resource_system sys(tasks, single_root());
	sys.set_desc_binary_hash_cache(cache);
	BOOST_TEST(read_hashes(sys));
	BOOST_TEST(!read_hashes(sys));

	const fs::file_time_type written = fs::last_write_time(bin());
	auto broken = file;
	broken[0] = std::byte{ 'X' };
	write_file(bin(), broken);
	fs::last_write_time(bin(), written);
	const std::uint64_t before = hashed();
	const auto refused = read(sys);
	BOOST_TEST((refused.error == res::desc_binary_error::header));
	BOOST_TEST(hashed() == before);
}

BOOST_AUTO_TEST_CASE(TheFileCacheIsWrittenAndReadBack)
{
	test_util::temp_dir project{ "res_desc_binary_hash_cache_file" };
	const auto key = some_key("res://a/b.geom.bin");
	{
		res::file_desc_binary_hash_cache cache(project.path());
		BOOST_TEST(!cache.verified(key));
		cache.remember(key);
		BOOST_TEST(cache.verified(key));
		BOOST_TEST(cache.dirty());
		BOOST_TEST(cache.flush());
		BOOST_TEST(!cache.dirty());
	}
	BOOST_TEST(fs::is_regular_file(project.path() / ".cache" / "desc_binary_verified.json"));
	res::file_desc_binary_hash_cache again(project.path());
	BOOST_TEST(again.verified(key));
	BOOST_TEST(!again.dirty());
	auto other = key;
	other.desc_stamp.write_time_ns += 1;
	BOOST_TEST(!again.verified(other));
	other = key;
	other.ranges_sha256 = std::string(64, 'b');
	BOOST_TEST(!again.verified(other));
}

BOOST_AUTO_TEST_CASE(TheFileCacheKeepsOneEntryPerBinaryFile)
{
	test_util::temp_dir project{ "res_desc_binary_hash_cache_one" };
	res::file_desc_binary_hash_cache cache(project.path());
	auto key = some_key("res://a/b.geom.bin");
	cache.remember(key);
	auto newer = key;
	newer.binary_stamp.size = 11;
	cache.remember(newer);
	BOOST_TEST(cache.size() == 1u);
	BOOST_TEST(!cache.verified(key));
	BOOST_TEST(cache.verified(newer));
}

BOOST_AUTO_TEST_CASE(AFileCacheNotExactlyAsWrittenIsIgnoredAndRewritten)
{
	const std::string entry =
		R"({"bin":"res://a/b.geom.bin","bin_size":10,"bin_mtime":5,"desc_size":20,"desc_mtime":6,"ranges":")" + std::string(64, 'a') + "\"";
	const std::vector<std::string> broken{
		"not json",
		"[]",
		R"({"version":2,"entries":[)" + entry + "}]}",
		R"({"version":1,"entries":[)" + entry + "}],\"more\":1}",
		R"({"version":1,"entries":[)" + entry + ",\"extra\":1}]}",
		R"({"version":1,"entries":[{"bin":"res://a/b.geom.bin","bin_size":10,"bin_mtime":5,"desc_size":20,"desc_mtime":6}]})",
		R"({"version":1,"entries":[{"bin":"res://a/b.geom.bin","bin_size":"10","bin_mtime":5,"desc_size":20,"desc_mtime":6,"ranges":")"
			+ std::string(64, 'a') + "\"}]}",
		R"({"version":1,"entries":[{"bin":"res://a/b.geom.bin","bin_size":10,"bin_mtime":5,"desc_size":20,"desc_mtime":6,"ranges":")"
			+ std::string(64, 'A') + "\"}]}",
	};
	const std::string good = R"({"version":1,"entries":[)" + entry + "}]}";
	for (const std::string& text : broken) {
		BOOST_TEST_CONTEXT(text) {
			test_util::temp_dir project{ "res_desc_binary_hash_cache_broken" };
			const fs::path path = project.path() / ".cache" / "desc_binary_verified.json";
			write_text(path, text);
			res::file_desc_binary_hash_cache cache(project.path());
			BOOST_TEST(cache.size() == 0u);
			BOOST_TEST(cache.dirty());
			BOOST_TEST(cache.flush());
			const auto written = boost::json::parse(read_text(path)).as_object();
			BOOST_TEST(written.at("version").to_number<std::int64_t>() == 1);
			BOOST_TEST(written.at("entries").as_array().empty());
		}
	}

	test_util::temp_dir project{ "res_desc_binary_hash_cache_good" };
	write_text(project.path() / ".cache" / "desc_binary_verified.json", good);
	{
		res::file_desc_binary_hash_cache cache(project.path());
		BOOST_TEST(cache.size() == 1u);
		BOOST_TEST(!cache.dirty());
	}
	// Over the size limit: ignored whole.
	res::file_desc_binary_hash_cache small(project.path(), 16);
	BOOST_TEST(small.size() == 0u);
	BOOST_TEST(small.dirty());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(ALinkAtTheFileCacheIsNotFollowed)
{
	test_util::temp_dir project{ "res_desc_binary_hash_cache_link" };
	const fs::path outside = project.path() / "outside.json";
	const std::string text = R"({"version":1,"entries":[{"bin":"res://a/b.geom.bin","bin_size":10,"bin_mtime":5,"desc_size":20,)"
		R"("desc_mtime":6,"ranges":")" + std::string(64, 'a') + "\"}]}";
	write_text(outside, text);
	fs::create_directories(project.path() / ".cache");
	std::error_code ec;
	fs::create_symlink(outside, project.path() / ".cache" / "desc_binary_verified.json", ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());

	res::file_desc_binary_hash_cache cache(project.path());
	BOOST_TEST(cache.size() == 0u);
	cache.remember(some_key("res://a/c.geom.bin"));
	BOOST_TEST(!cache.flush());
	BOOST_TEST(read_text(outside) == text);
	BOOST_TEST(fs::is_symlink(project.path() / ".cache" / "desc_binary_verified.json"));
}
#endif

BOOST_AUTO_TEST_CASE(TheFileCacheDropsTheOldestPastItsLimit)
{
	test_util::temp_dir project{ "res_desc_binary_hash_cache_limit" };
	res::file_desc_binary_hash_cache cache(project.path());
	for (std::size_t i = 0; i <= res::file_desc_binary_hash_cache::max_entries; ++i) {
		cache.remember(some_key("res://a/" + std::to_string(i) + ".geom.bin"));
	}
	BOOST_TEST(cache.size() == res::file_desc_binary_hash_cache::max_entries);
	BOOST_TEST(!cache.verified(some_key("res://a/0.geom.bin")));
	BOOST_TEST(cache.verified(some_key("res://a/1.geom.bin")));
	BOOST_TEST(cache.verified(some_key(std::format("res://a/{}.geom.bin", res::file_desc_binary_hash_cache::max_entries))));
}

BOOST_AUTO_TEST_SUITE_END()
