#include <boost/test/unit_test.hpp>
#include "edt_thumbnail_cache.h"
#include "test_temp_dir.hpp"
#include <snk/image/img_image.h>
#include <snk/resource/res_asset_walk.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <vector>

// #1192 PR 2: the disk cache of thumbnails: its entries, the key that keeps a stale one from being shown, the bounds of
// the folder, and that the folder it lives in is no asset.

namespace {

namespace fs = std::filesystem;

std::string png_bytes(int width, int height, unsigned char fill)
{
	const std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, fill);
	std::vector<std::byte> png;
	img::image::write_to_memory(img::image_type::png, pixels.data(), width, height, 3, png);
	return std::string(reinterpret_cast<const char*>(png.data()), png.size());
}

struct cache_env
{
	explicit cache_env(const char* name) : layer(std::string(name) + "_layer"), cache_dir(std::string(name) + "_cache") {}

	fs::path layer_root() const { return fs::weakly_canonical(layer.path()); }
	fs::path cache_root() const { return fs::weakly_canonical(cache_dir.path()); }
	fs::path thumbs() const { return cache_root() / "thumbnails"; }

	edt::thumbnail_decode_fn counting_decoder()
	{
		return [this](const fs::path& root, const fs::path& relative, std::optional<platform::beneath::file_stamp>& stamp) {
			++decodes;
			return edt::decode_thumbnail_file(root, relative, stamp);
		};
	}

	std::vector<fs::path> entries() const
	{
		std::vector<fs::path> found;
		std::error_code ec;
		for (fs::directory_iterator it(thumbs(), ec), end; !ec && it != end; it.increment(ec)) {
			if (it->path().extension() == ".thumb") found.push_back(it->path());
		}
		std::ranges::sort(found);
		return found;
	}

	test_util::temp_dir layer;
	test_util::temp_dir cache_dir;
	int decodes = 0;
};

edt::thumbnail_pixels sample_pixels()
{
	edt::thumbnail_pixels pixels;
	pixels.width = 3;
	pixels.height = 2;
	for (int i = 0; i < 24; ++i) pixels.rgba.push_back(static_cast<unsigned char>(i * 10));
	return pixels;
}

platform::beneath::file_stamp stamp_of(std::uint64_t size)
{
	platform::beneath::file_stamp stamp;
	stamp.size = size;
	stamp.write_time_ns = 5;
	stamp.change_time_ns = 6;
	stamp.file_id = 7;
	stamp.device = 8;
	return stamp;
}

}

BOOST_AUTO_TEST_SUITE(EdtThumbnailCache)

BOOST_AUTO_TEST_CASE(AnEntryRoundTrips_AndOnlyForItsOwnKey)
{
	const std::string key = edt::thumbnail_disk_cache::key_of("tex/a.png", stamp_of(10));
	BOOST_REQUIRE(edt::thumbnail_disk_cache::is_entry_name(key + ".thumb"));
	const edt::thumbnail_pixels pixels = sample_pixels();
	const std::vector<std::byte> bytes = edt::thumbnail_disk_cache::encode_entry(key, pixels);
	const auto back = edt::thumbnail_disk_cache::decode_entry(key, bytes);
	BOOST_REQUIRE(back.has_value());
	BOOST_TEST(back->width == 3);
	BOOST_TEST(back->height == 2);
	BOOST_TEST((back->rgba == pixels.rgba));

	const std::string other = edt::thumbnail_disk_cache::key_of("tex/a.png", stamp_of(11));
	BOOST_TEST(other != key);
	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(other, bytes).has_value());
}

BOOST_AUTO_TEST_CASE(AnythingButAnExactEntry_IsNotOne)
{
	const std::string key = edt::thumbnail_disk_cache::key_of("a.png", stamp_of(1));
	const std::vector<std::byte> good = edt::thumbnail_disk_cache::encode_entry(key, sample_pixels());

	auto truncated = good;
	truncated.pop_back();
	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(key, truncated).has_value());

	auto longer = good;
	longer.push_back(std::byte{ 0 });
	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(key, longer).has_value());

	auto bad_magic = good;
	bad_magic[0] = std::byte{ 'X' };
	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(key, bad_magic).has_value());

	auto bad_version = good;
	bad_version[4] = std::byte{ 9 };
	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(key, bad_version).has_value());

	auto huge_width = good;
	huge_width[8 + 64] = std::byte{ 200 };
	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(key, huge_width).has_value());

	BOOST_TEST(!edt::thumbnail_disk_cache::decode_entry(key, std::span<const std::byte>{}).has_value());
	BOOST_TEST(!edt::thumbnail_disk_cache::is_entry_name("notes.txt"));
	BOOST_TEST(!edt::thumbnail_disk_cache::is_entry_name(std::string(64, 'g') + ".thumb"));
}

BOOST_AUTO_TEST_CASE(TheCacheIsKeptOnlyForATrustedProject)
{
	const std::optional<fs::path> dir = fs::path("proj/.cache");
	BOOST_TEST((edt::thumbnail_cache_location(dir, true) == dir));
	BOOST_TEST(!edt::thumbnail_cache_location(dir, false).has_value());
	BOOST_TEST(!edt::thumbnail_cache_location(std::nullopt, true).has_value());
}

BOOST_AUTO_TEST_CASE(TheKeyFollowsThePathAndEveryPartOfTheStamp)
{
	const platform::beneath::file_stamp base = stamp_of(10);
	const std::string key = edt::thumbnail_disk_cache::key_of("a.png", base);
	BOOST_TEST(key == edt::thumbnail_disk_cache::key_of("a.png", base));
	BOOST_TEST(key != edt::thumbnail_disk_cache::key_of("b.png", base));
	auto changed = base; changed.size = 11;
	BOOST_TEST(key != edt::thumbnail_disk_cache::key_of("a.png", changed));
	changed = base; changed.write_time_ns = 1;
	BOOST_TEST(key != edt::thumbnail_disk_cache::key_of("a.png", changed));
	changed = base; changed.change_time_ns = 1;
	BOOST_TEST(key != edt::thumbnail_disk_cache::key_of("a.png", changed));
	changed = base; changed.file_id = 1;
	BOOST_TEST(key != edt::thumbnail_disk_cache::key_of("a.png", changed));
	changed = base; changed.device = 1;
	BOOST_TEST(key != edt::thumbnail_disk_cache::key_of("a.png", changed));
}

BOOST_AUTO_TEST_CASE(AFileIsDecodedOnce_AndThenComesFromTheCache)
{
	cache_env env("thumbcache_hit");
	env.layer.write("tex/a.png", png_bytes(100, 50, 120));
	const edt::thumbnail_disk_cache cache(env.cache_root());

	const auto first = cache.load_or_decode(env.layer_root(), "tex/a.png", env.counting_decoder());
	BOOST_REQUIRE(first.pixels.has_value());
	BOOST_TEST(env.decodes == 1);
	BOOST_TEST(cache.writes() == 1u);
	BOOST_TEST(env.entries().size() == 1u);

	const auto second = cache.load_or_decode(env.layer_root(), "tex/a.png", env.counting_decoder());
	BOOST_REQUIRE(second.pixels.has_value());
	BOOST_TEST(env.decodes == 1);
	BOOST_TEST(cache.hits() == 1u);
	BOOST_TEST(second.pixels->width == first.pixels->width);
	BOOST_TEST((second.pixels->rgba == first.pixels->rgba));
}

BOOST_AUTO_TEST_CASE(AChangedFile_IsDecodedAgain_NeverShownFromTheOldEntry)
{
	cache_env env("thumbcache_changed");
	env.layer.write("a.png", png_bytes(100, 50, 40));
	const edt::thumbnail_disk_cache cache(env.cache_root());
	const auto before = cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder());
	BOOST_REQUIRE(before.pixels.has_value());

	env.layer.write("a.png", png_bytes(60, 60, 200));
	const auto after = cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder());
	BOOST_REQUIRE(after.pixels.has_value());
	BOOST_TEST(env.decodes == 2);
	BOOST_TEST(after.pixels->width == 60);
	BOOST_TEST(after.pixels->rgba[0] == 200);
	BOOST_TEST(env.entries().size() == 2u);
}

BOOST_AUTO_TEST_CASE(ABrokenEntry_IsAMiss_AndIsReplaced)
{
	cache_env env("thumbcache_broken");
	env.layer.write("a.png", png_bytes(32, 32, 77));
	const edt::thumbnail_disk_cache cache(env.cache_root());
	BOOST_REQUIRE(cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder()).pixels.has_value());
	const auto entries = env.entries();
	BOOST_REQUIRE(entries.size() == 1u);
	fs::resize_file(entries[0], 20);

	const auto again = cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder());
	BOOST_REQUIRE(again.pixels.has_value());
	BOOST_TEST(env.decodes == 2);
	BOOST_TEST(fs::file_size(entries[0]) > 20u);
	BOOST_TEST(cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder()).pixels.has_value());
	BOOST_TEST(env.decodes == 2);
}

BOOST_AUTO_TEST_CASE(AFailedDecode_IsNotKept)
{
	cache_env env("thumbcache_failed");
	env.layer.write("bad.png", "this is not a png");
	const edt::thumbnail_disk_cache cache(env.cache_root());
	const auto failed = cache.load_or_decode(env.layer_root(), "bad.png", env.counting_decoder());
	BOOST_TEST(!failed.pixels.has_value());
	BOOST_TEST(!failed.error.empty());
	BOOST_TEST(cache.writes() == 0u);
	BOOST_TEST(env.entries().empty());
	BOOST_TEST(!cache.load_or_decode(env.layer_root(), "missing.png", env.counting_decoder()).pixels.has_value());
}

BOOST_AUTO_TEST_CASE(AFileThatChangesWhileItIsDecoded_IsNotCachedUnderTheOldStamp)
{
	cache_env env("thumbcache_race");
	env.layer.write("a.png", png_bytes(40, 40, 10));
	const edt::thumbnail_disk_cache cache(env.cache_root());
	const edt::thumbnail_decode_fn racing = [](const fs::path& root, const fs::path& relative, std::optional<platform::beneath::file_stamp>& stamp) {
		auto decoded = edt::decode_thumbnail_file(root, relative, stamp);
		if (stamp) stamp->size += 1;
		return decoded;
	};
	BOOST_TEST(cache.load_or_decode(env.layer_root(), "a.png", racing).pixels.has_value());
	BOOST_TEST(cache.writes() == 0u);
	BOOST_TEST(env.entries().empty());
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(ALinkAtTheEntryName_IsNotFollowed)
{
	cache_env env("thumbcache_link");
	env.layer.write("a.png", png_bytes(32, 32, 5));
	const edt::thumbnail_disk_cache cache(env.cache_root());
	BOOST_REQUIRE(cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder()).pixels.has_value());
	const auto entries = env.entries();
	BOOST_REQUIRE(entries.size() == 1u);

	const fs::path elsewhere = env.cache_root() / "kept.bin";
	fs::rename(entries[0], elsewhere);
	std::error_code ec;
	fs::create_symlink(elsewhere, entries[0], ec);
	if (ec) {
		BOOST_TEST_MESSAGE("cannot make a symlink here: skipped");
		return;
	}
	BOOST_TEST(cache.load_or_decode(env.layer_root(), "a.png", env.counting_decoder()).pixels.has_value());
	BOOST_TEST(env.decodes == 2);
}
#endif

BOOST_AUTO_TEST_CASE(Pruning_KeepsTheNewest_RemovesStaleLeftovers_AndTouchesNothingElse)
{
	cache_env env("thumbcache_prune");
	edt::thumbnail_disk_cache::limits limits;
	limits.max_entries = 2;
	limits.prune_every_writes = 3;
	const edt::thumbnail_disk_cache cache(env.cache_root(), limits);
	BOOST_TEST(cache.prune_due());

	for (int i = 0; i < 4; ++i) {
		env.layer.write(std::format("p{}.png", i), png_bytes(16 + i, 16, static_cast<unsigned char>(i)));
		BOOST_REQUIRE(cache.load_or_decode(env.layer_root(), std::format("p{}.png", i), env.counting_decoder()).pixels.has_value());
	}
	std::vector<fs::path> entries = env.entries();
	BOOST_REQUIRE(entries.size() == 4u);
	const auto now = fs::file_time_type::clock::now();
	std::vector<std::pair<fs::path, fs::file_time_type>> aged;
	for (std::size_t i = 0; i < entries.size(); ++i) {
		const auto when = now - std::chrono::minutes(10 * (static_cast<int>(i) + 1));
		fs::last_write_time(entries[i], when);
		aged.emplace_back(entries[i], when);
	}
	std::ranges::sort(aged, [](const auto& a, const auto& b) { return a.second > b.second; });

	env.cache_dir.write("thumbnails/notes.txt", "mine");
	const std::string tmp_name = std::string(64, 'a');
	env.cache_dir.write("thumbnails/" + tmp_name + ".1.tmp", "old");
	env.cache_dir.write("thumbnails/" + tmp_name + ".2.tmp", "fresh");
	fs::last_write_time(env.thumbs() / (tmp_name + ".1.tmp"), now - std::chrono::hours(3));

	BOOST_TEST(cache.prune_due());
	const std::size_t removed = cache.prune();
	BOOST_TEST(removed == 3u);
	BOOST_TEST(!cache.prune_due());

	const std::vector<fs::path> left = env.entries();
	BOOST_REQUIRE(left.size() == 2u);
	for (std::size_t i = 0; i < 2; ++i) {
		BOOST_TEST(fs::exists(aged[i].first));
	}
	BOOST_TEST(fs::exists(env.thumbs() / "notes.txt"));
	BOOST_TEST(fs::exists(env.thumbs() / (tmp_name + ".2.tmp")));
	BOOST_TEST(!fs::exists(env.thumbs() / (tmp_name + ".1.tmp")));
}

BOOST_AUTO_TEST_CASE(NoCacheFolderYet_PruningIsHarmless)
{
	cache_env env("thumbcache_empty");
	const edt::thumbnail_disk_cache cache(env.cache_root());
	BOOST_TEST(cache.prune() == 0u);
	BOOST_TEST(!fs::exists(env.thumbs()));
}

BOOST_AUTO_TEST_CASE(TheCacheFolderOfTheProject_IsNoAsset)
{
	cache_env env("thumbcache_asset");
	env.layer.write("shown.png", png_bytes(8, 8, 1));
	const fs::path project_cache = env.layer_root() / ".cache";
	const edt::thumbnail_disk_cache cache(project_cache);
	BOOST_REQUIRE(cache.load_or_decode(env.layer_root(), "shown.png", env.counting_decoder()).pixels.has_value());
	BOOST_REQUIRE(fs::exists(project_cache / "thumbnails"));

	std::vector<std::string> walked;
	for (const res::asset_walk_file& file : res::walk_asset_layers({ env.layer_root() }, {}, false)) {
		walked.push_back(file.relative.generic_string());
	}
	BOOST_TEST((walked == std::vector<std::string>{ "shown.png" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()
