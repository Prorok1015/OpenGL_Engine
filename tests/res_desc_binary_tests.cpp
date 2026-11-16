#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_vfs_test_hook.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

// A desc's binary file (EPIC-79, #1068): the format, its checks, the save through a staged file and the layer it is
// read from.

namespace {

namespace fs = std::filesystem;

const std::vector<float> k_positions{ 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
const std::vector<std::uint32_t> k_indices{ 0, 1, 1 };
const std::vector<float> k_positions_v2{ 9.0f, 8.0f, 7.0f };
const std::vector<std::uint32_t> k_indices_v2{ 0, 0, 0 };
const std::vector<float> k_positions_v3{ 6.0f, 6.0f, 6.0f, 1.0f, 1.0f, 1.0f };
const std::vector<std::uint32_t> k_indices_v3{ 1, 0, 1 };

std::array<res::binary_chunk, 2> chunks_of(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices)
{
	return { res::binary_chunk{ std::as_bytes(std::span(positions)), "f32x3" },
		res::binary_chunk{ std::as_bytes(std::span(indices)), "u32" } };
}

std::vector<std::byte> encoded(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices,
	std::vector<res::binary_range>& ranges)
{
	std::string error;
	const auto chunks = chunks_of(positions, indices);
	auto file = res::desc_binary::encode(chunks, ranges, error);
	BOOST_REQUIRE_MESSAGE(file.has_value(), error);
	return *file;
}

std::vector<std::byte> read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::vector<char> chars{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::vector<std::byte> out(chars.size());
	for (std::size_t i = 0; i < chars.size(); ++i) {
		out[i] = static_cast<std::byte>(chars[i]);
	}
	return out;
}

void write_file(const fs::path& path, const std::vector<std::byte>& bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> bytes_of(const std::string& text)
{
	const auto view = std::as_bytes(std::span(text));
	return { view.begin(), view.end() };
}

std::string desc_text(const std::vector<res::binary_range>& ranges)
{
	boost::json::object desc;
	desc["vertices"] = res::to_json(ranges[0]);
	desc["indices"] = res::to_json(ranges[1]);
	return boost::json::serialize(desc);
}

// res:// over two content roots: `top` is the first (highest priority, writable), `lower` below it.
struct two_layers
{
	test_util::temp_dir dir{ "res_desc_binary" };
	fs::path top = dir.path() / "top";
	fs::path lower = dir.path() / "lower";
	fs::path cache = dir.path() / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	const res::tag desc{ "res://a/robot.geom.desc" };

	two_layers()
	{
		fs::create_directories(top / "a");
		fs::create_directories(lower / "a");
		fs::create_directories(cache / "memory");
		fs::create_directories(cache / "user_profile");
	}

	res::mount_table table() const
	{
		return res::resource_system::project_mount_table({}, { top, lower }, cache, cache / "user_profile");
	}

	// res:// over `top` alone.
	res::mount_table single_root_table() const
	{
		return res::resource_system::project_mount_table({}, { top }, cache, cache / "user_profile");
	}

	res::mount_table read_only_table() const
	{
		auto mounts = table();
		for (auto& mounted : mounts) {
			if (mounted.protocol == "res") {
				mounted.access = res::mount_access::read_only;
			}
		}
		return mounts;
	}

	fs::path bin() const { return top / "a" / "robot.geom.bin"; }
	fs::path staged() const { return top / "a" / "robot.geom.bin.new"; }
};

// A whole save under its lock: steps 1 to 3.
res::desc_binary_written save(res::resource_system& sys, const res::tag& desc, const std::vector<float>& positions,
	const std::vector<std::uint32_t>& indices)
{
	const res::desc_binary::save_lock lock(desc);
	auto written = res::desc_binary::write_pending(sys, desc, chunks_of(positions, indices));
	BOOST_REQUIRE_MESSAGE(written, written.error);
	BOOST_REQUIRE(sys.store_sync(desc, bytes_of(desc_text(written.ranges))));
	const auto committed = res::desc_binary::commit_pending(sys, desc, written);
	BOOST_REQUIRE_MESSAGE(committed, committed.error);
	return written;
}

// Steps 1 and 2 of a save, then the crash.
res::desc_binary_written save_cut_before_commit(res::resource_system& sys, const res::tag& desc,
	const std::vector<float>& positions, const std::vector<std::uint32_t>& indices)
{
	auto written = res::desc_binary::write_pending(sys, desc, chunks_of(positions, indices));
	BOOST_REQUIRE_MESSAGE(written, written.error);
	BOOST_REQUIRE(sys.store_sync(desc, bytes_of(desc_text(written.ranges))));
	return written;
}

std::vector<float> positions_of(const res::desc_binary_data& data)
{
	std::vector<float> positions;
	BOOST_REQUIRE(!data.ranges.empty());
	BOOST_TEST(res::copy_binary_values(data.ranges[0], positions));
	return positions;
}

}

BOOST_AUTO_TEST_SUITE(ResDescBinaryTests)

BOOST_AUTO_TEST_CASE(TheBinaryFileIsNamedAfterTheDesc)
{
	BOOST_TEST(res::desc_binary::binary_tag(res::tag{ "res://a/robot.geom.desc" })->view() == "res://a/robot.geom.bin");
	BOOST_TEST(res::desc_binary::staged_tag(res::tag{ "res://a/robot.geom.desc" })->view() == "res://a/robot.geom.bin.new");
	BOOST_TEST(!res::desc_binary::binary_tag(res::tag{ "res://a/robot.desc" }).has_value());
	BOOST_TEST(!res::desc_binary::binary_tag(res::tag{ "res://a/.geom.desc" }).has_value());
	BOOST_TEST(!res::desc_binary::binary_tag(res::tag{ "res://a/robot.geom.bin" }).has_value());
}

BOOST_AUTO_TEST_CASE(TheBinaryFilesOfADescAreNoAssets)
{
	const res::tag desc{ "res://a/robot.geom.desc" };
	BOOST_TEST(res::is_ignored_asset_name(res::desc_binary::binary_tag(desc)->name()));
	BOOST_TEST(res::is_ignored_asset_name(res::desc_binary::staged_tag(desc)->name()));
	BOOST_TEST(!res::is_ignored_asset_name(desc.name()));
	// A plain .bin can be a user's asset.
	BOOST_TEST(!res::is_ignored_asset_name("firmware.bin"));
}

BOOST_AUTO_TEST_CASE(LayoutsComeFromAClosedVocabulary)
{
	for (const char* good : { "u8", "u32", "f32", "f32x1", "f32x3", "u32x32" }) {
		BOOST_TEST(res::parse_binary_layout(good).has_value(), good);
	}
	for (const char* bad : { "", "u16", "f64", "F32", "u8x4", "f32x0", "f32x33", "f32x03", "f32x", "f32y3", "f32x3 ", "u32x100" }) {
		BOOST_TEST(!res::parse_binary_layout(bad).has_value(), bad);
	}
	BOOST_TEST(res::parse_binary_layout("f32x3")->element_bytes() == 12u);
}

BOOST_AUTO_TEST_CASE(RangesRoundTripThroughJson)
{
	std::vector<res::binary_range> ranges;
	encoded(k_positions, k_indices, ranges);
	std::string error;
	const auto back = res::binary_range_from_json(boost::json::value(res::to_json(ranges[0])), error);
	BOOST_REQUIRE_MESSAGE(back.has_value(), error);
	BOOST_TEST(back->offset == ranges[0].offset);
	BOOST_TEST(back->size == ranges[0].size);
	BOOST_TEST(back->sha256 == ranges[0].sha256);
	BOOST_TEST(back->layout == ranges[0].layout);

	auto broken = res::to_json(ranges[0]);
	broken["layout"] = "f64";
	BOOST_TEST(!res::binary_range_from_json(boost::json::value(broken), error).has_value());
	broken = res::to_json(ranges[0]);
	broken["offset"] = -1;
	BOOST_TEST(!res::binary_range_from_json(boost::json::value(broken), error).has_value());
	broken = res::to_json(ranges[0]);
	broken["sha256"] = "abc";
	BOOST_TEST(!res::binary_range_from_json(boost::json::value(broken), error).has_value());
}

BOOST_AUTO_TEST_CASE(OffsetsAndSizesAreIntegersUpTo2Pow53)
{
	std::vector<res::binary_range> ranges;
	encoded(k_positions, k_indices, ranges);
	const std::string sha = ranges[0].sha256;
	const auto range_with = [&](std::string_view offset, std::string_view size) {
		return boost::json::parse(std::format(R"({{"offset":{},"size":{},"sha256":"{}","layout":"u8"}})", offset, size, sha));
	};
	std::string error;
	BOOST_TEST(res::binary_range_from_json(range_with("16", "9007199254740992"), error).has_value());
	for (const auto& [offset, size] : std::vector<std::pair<std::string_view, std::string_view>>{
			{ "16.5", "4" }, { "16", "4.0" }, { "1e3", "4" }, { "16", "9007199254740993" },
			{ "18446744073709551615", "4" }, { "16", "18446744073709551616" }, { "\"16\"", "4" }, { "16", "\"4\"" } }) {
		BOOST_TEST_CONTEXT(offset << ", " << size) {
			BOOST_TEST(!res::binary_range_from_json(range_with(offset, size), error).has_value());
		}
	}
}

BOOST_AUTO_TEST_CASE(TooManyRangesAreRefused)
{
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	// Zero-size ranges are allowed, so only the count refuses these.
	const std::vector<res::binary_range> many(res::desc_binary::max_ranges + 1,
		res::binary_range{ res::desc_binary::header_bytes, 0, ranges[0].sha256, "u8" });
	const auto refused = res::desc_binary::parse("f.bin", file, many, false);
	BOOST_TEST((refused.error == res::desc_binary_error::range));
	const std::vector<res::binary_range> enough(res::desc_binary::max_ranges,
		res::binary_range{ res::desc_binary::header_bytes, 0, ranges[0].sha256, "u8" });
	BOOST_TEST(static_cast<bool>(res::desc_binary::parse("f.bin", file, enough, false)));
}

BOOST_FIXTURE_TEST_CASE(WritePendingCommitAndReadRoundTrip, two_layers)
{
	res::resource_system sys(tasks, table());
	const auto chunks = chunks_of(k_positions, k_indices);
	const auto written = res::desc_binary::write_pending(sys, desc, chunks);
	BOOST_REQUIRE_MESSAGE(written, written.error);
	BOOST_REQUIRE(written.ranges.size() == 2u);
	BOOST_TEST(written.ranges[0].offset % res::desc_binary::range_alignment == 0u);
	BOOST_TEST(written.ranges[1].offset % res::desc_binary::range_alignment == 0u);
	BOOST_TEST(fs::exists(top / "a" / "robot.geom.bin.new"));
	BOOST_TEST(!fs::exists(top / "a" / "robot.geom.bin"));

	BOOST_REQUIRE(sys.store_sync(desc, bytes_of(desc_text(written.ranges))));
	const auto committed = res::desc_binary::commit_pending(sys, desc, written);
	BOOST_REQUIRE_MESSAGE(committed, committed.error);
	BOOST_TEST(fs::exists(top / "a" / "robot.geom.bin"));
	BOOST_TEST(!fs::exists(top / "a" / "robot.geom.bin.new"));

	const auto data = res::desc_binary::read(sys, desc, written.ranges, true);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(!data.from_staged);
	BOOST_REQUIRE(data.ranges.size() == 2u);
	std::vector<float> positions;
	std::vector<std::uint32_t> indices;
	BOOST_TEST(res::copy_binary_values(data.ranges[0], positions));
	BOOST_TEST(res::copy_binary_values(data.ranges[1], indices));
	BOOST_TEST((positions == k_positions));
	BOOST_TEST((indices == k_indices));
}

BOOST_AUTO_TEST_CASE(AWrongMagicOrVersionIsRefused)
{
	std::vector<res::binary_range> ranges;
	auto file = encoded(k_positions, k_indices, ranges);
	auto wrong_magic = file;
	wrong_magic[0] = std::byte{ 'X' };
	BOOST_TEST((res::desc_binary::parse("f.bin", wrong_magic, ranges, true).error == res::desc_binary_error::header));
	auto wrong_version = file;
	wrong_version[4] = std::byte{ 2 };
	const auto refused = res::desc_binary::parse("f.bin", wrong_version, ranges, true);
	BOOST_TEST((refused.error == res::desc_binary_error::header));
	BOOST_TEST(refused.message.find("f.bin") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ATruncatedFileIsRefused)
{
	std::vector<res::binary_range> ranges;
	auto file = encoded(k_positions, k_indices, ranges);
	auto short_of_header = file;
	short_of_header.resize(5);
	BOOST_TEST((res::desc_binary::parse("f.bin", short_of_header, ranges, true).error == res::desc_binary_error::header));
	auto short_of_data = file;
	short_of_data.resize(static_cast<std::size_t>(ranges[1].offset + 2));
	const auto refused = res::desc_binary::parse("f.bin", short_of_data, ranges, true);
	BOOST_TEST((refused.error == res::desc_binary_error::range));
	BOOST_TEST(refused.message.find("range 1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnOffsetPlusSizeOverflowIsRefused)
{
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	ranges[1].offset = std::numeric_limits<std::uint64_t>::max() - 3;
	BOOST_TEST((res::desc_binary::parse("f.bin", file, ranges, true).error == res::desc_binary_error::range));
}

BOOST_AUTO_TEST_CASE(ARangeOutsideTheFileOrInTheHeaderIsRefused)
{
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	auto outside = ranges;
	outside[1].offset = file.size();
	BOOST_TEST((res::desc_binary::parse("f.bin", file, outside, true).error == res::desc_binary_error::range));
	auto in_header = ranges;
	in_header[0].offset = 4;
	BOOST_TEST((res::desc_binary::parse("f.bin", file, in_header, false).error == res::desc_binary_error::range));
}

BOOST_AUTO_TEST_CASE(OverlappingRangesAreRefused)
{
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	ranges[1].offset = ranges[0].offset + 4;
	const auto refused = res::desc_binary::parse("f.bin", file, ranges, false);
	BOOST_TEST((refused.error == res::desc_binary_error::range));
	BOOST_TEST(refused.message.find("overlaps") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnUnknownLayoutOrASizeOffTheLayoutIsRefused)
{
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	auto unknown = ranges;
	unknown[0].layout = "f64";
	BOOST_TEST((res::desc_binary::parse("f.bin", file, unknown, false).error == res::desc_binary_error::layout));
	auto off_layout = ranges;
	off_layout[0].size = 20;
	BOOST_TEST((res::desc_binary::parse("f.bin", file, off_layout, false).error == res::desc_binary_error::layout));

	std::vector<res::binary_range> none;
	std::string error;
	const std::vector<std::byte> five(5);
	const std::array chunks{ res::binary_chunk{ five, "u32" } };
	BOOST_TEST(!res::desc_binary::encode(chunks, none, error).has_value());
}

BOOST_AUTO_TEST_CASE(AHashMismatchIsRefusedOnlyWhenHashesAreChecked)
{
	std::vector<res::binary_range> ranges;
	auto file = encoded(k_positions, k_indices, ranges);
	file[static_cast<std::size_t>(ranges[0].offset)] ^= std::byte{ 0x01 };
	BOOST_TEST((res::desc_binary::parse("f.bin", file, ranges, true).error == res::desc_binary_error::hash));
	BOOST_TEST(static_cast<bool>(res::desc_binary::parse("f.bin", file, ranges, false)));
}

BOOST_FIXTURE_TEST_CASE(AnOverLimitFileIsRefusedBeforeItIsRead, two_layers)
{
	res::resource_system sys(tasks, table());
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	write_file(top / "a" / "robot.geom.bin", file);

	// The ranges fit the limit, the file does not.
	const std::vector<res::binary_range> first{ ranges[0] };
	const auto by_file = res::desc_binary::read(sys, desc, first, true, ranges[0].offset + ranges[0].size);
	BOOST_TEST((by_file.error == res::desc_binary_error::too_large));
	// The ranges alone exceed it: refused without touching the file.
	const auto by_ranges = res::desc_binary::read(sys, desc, ranges, true, ranges[0].offset);
	BOOST_TEST((by_ranges.error == res::desc_binary_error::too_large));
}

BOOST_FIXTURE_TEST_CASE(ASaveCutBeforeTheCommitIsReadFromTheStagedFile, two_layers)
{
	res::resource_system sys(tasks, table());
	save(sys, desc, k_positions, k_indices);
	const auto old_bin = read_file(bin());
	const auto second = save_cut_before_commit(sys, desc, k_positions_v2, k_indices_v2);

	// Without hash checks asked for: the staged file makes the reader check them. A plain read writes nothing.
	const auto data = res::desc_binary::read(sys, desc, second.ranges, false);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(data.from_staged);
	BOOST_TEST((positions_of(data) == k_positions_v2));
	BOOST_TEST((read_file(staged()) == second.file));
	BOOST_TEST((read_file(bin()) == old_bin));

	// A read in commit mode finishes the save: the top layer is writable.
	const auto committed = res::desc_binary::read(sys, desc, second.ranges, false, res::desc_binary::max_file_bytes,
		res::desc_binary_recovery::commit);
	BOOST_REQUIRE_MESSAGE(committed, committed.message);
	BOOST_TEST(committed.from_staged);
	BOOST_TEST(!fs::exists(staged()));
	BOOST_TEST((read_file(bin()) == second.file));
	const auto again = res::desc_binary::read(sys, desc, second.ranges, true);
	BOOST_REQUIRE_MESSAGE(again, again.message);
	BOOST_TEST(!again.from_staged);
}

BOOST_FIXTURE_TEST_CASE(AFirstSaveCutBeforeTheCommitIsFinishedByACommittingRead, two_layers)
{
	res::resource_system sys(tasks, table());
	const auto written = save_cut_before_commit(sys, desc, k_positions, k_indices);

	const auto data = res::desc_binary::read(sys, desc, written.ranges, true, res::desc_binary::max_file_bytes,
		res::desc_binary_recovery::commit);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(data.from_staged);
	BOOST_TEST((read_file(bin()) == written.file));
}

BOOST_FIXTURE_TEST_CASE(ASaveCutBeforeTheDescWriteReadsTheOldPair, two_layers)
{
	res::resource_system sys(tasks, table());
	const auto first = save(sys, desc, k_positions, k_indices);
	const auto second = res::desc_binary::write_pending(sys, desc, chunks_of(k_positions_v2, k_indices_v2));
	BOOST_REQUIRE_MESSAGE(second, second.error);

	const auto data = res::desc_binary::read(sys, desc, first.ranges, false);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(!data.from_staged);
	BOOST_TEST((positions_of(data) == k_positions));
	BOOST_TEST(fs::exists(staged()));

	// Only a committing read removes the stale staged file.
	const auto committed = res::desc_binary::read(sys, desc, first.ranges, false, res::desc_binary::max_file_bytes,
		res::desc_binary_recovery::commit);
	BOOST_REQUIRE_MESSAGE(committed, committed.message);
	BOOST_TEST(!fs::exists(staged()));
	BOOST_TEST((read_file(bin()) == first.file));
}

BOOST_FIXTURE_TEST_CASE(AStagedFileLeftAfterTheBinaryWriteIsIgnored, two_layers)
{
	res::resource_system sys(tasks, table());
	save(sys, desc, k_positions, k_indices);
	const auto second = save_cut_before_commit(sys, desc, k_positions_v2, k_indices_v2);
	// The binary file was replaced, then the crash came before the staged file was removed.
	write_file(bin(), second.file);

	const auto data = res::desc_binary::read(sys, desc, second.ranges, false);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(!data.from_staged);
	BOOST_TEST((positions_of(data) == k_positions_v2));
	BOOST_TEST(fs::exists(staged()));
}

// The race of the review of #1074: a recovery has verified staged v2, a save of v3 stages its file and writes its desc,
// then the recovery commits. Called step by step, in that order.
BOOST_FIXTURE_TEST_CASE(ASaveStartedDuringARecoveryKeepsItsStagedFile, two_layers)
{
	res::resource_system sys(tasks, table());
	const auto first = save(sys, desc, k_positions, k_indices);
	const auto v2 = save_cut_before_commit(sys, desc, k_positions_v2, k_indices_v2);

	// The recovery's check: staged v2 matches desc v2.
	const auto verified = read_file(staged());
	BOOST_REQUIRE(static_cast<bool>(res::desc_binary::parse("staged", verified, v2.ranges, true)));
	res::desc_binary_written recovery;
	recovery.ranges = v2.ranges;
	recovery.file = verified;

	// The save of v3, up to its commit.
	const auto v3 = save_cut_before_commit(sys, desc, k_positions_v3, k_indices_v3);

	// The recovery's commit of what it verified: refused, nothing touched.
	const auto late = res::desc_binary::commit_pending(sys, desc, recovery);
	BOOST_TEST(!late);
	BOOST_TEST((read_file(staged()) == v3.file));
	BOOST_TEST((read_file(bin()) == first.file));

	// The save's own commit then succeeds: desc v3 with .bin v3.
	const auto committed = res::desc_binary::commit_pending(sys, desc, v3);
	BOOST_REQUIRE_MESSAGE(committed, committed.error);
	BOOST_TEST(!fs::exists(staged()));
	BOOST_TEST((read_file(bin()) == v3.file));
	const auto data = res::desc_binary::read(sys, desc, v3.ranges, true);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(!data.from_staged);
	BOOST_TEST((positions_of(data) == k_positions_v3));
}

// The same race one step later: the staged file changes while the binary file is written.
BOOST_AUTO_TEST_CASE(ACommitKeepsAStagedFileRewrittenWhileTheTargetIsWritten)
{
	test_util::temp_dir dir{ "res_desc_binary_commit" };
	fs::create_directories(dir.path() / "a");
	res::vfs_resolver resolver("res", { dir.path() });
	const res::tag staged{ "res://a/robot.geom.bin.new" };
	const res::tag target{ "res://a/robot.geom.bin" };
	const auto v2 = bytes_of("staged v2");
	const auto v3 = bytes_of("staged v3");
	write_file(dir.path() / "a" / "robot.geom.bin.new", v2);

	res::detail::write_hook_access::set(resolver, [&](res::detail::write_stage stage, const fs::path&) {
		if (stage == res::detail::write_stage::before_rename) {
			write_file(dir.path() / "a" / "robot.geom.bin.new", v3);
		}
	});
	const auto committed = resolver.commit_staged(staged, target, v2);
	res::detail::write_hook_access::set(resolver, nullptr);
	BOOST_REQUIRE_MESSAGE(committed, committed.error);
	BOOST_TEST((read_file(dir.path() / "a" / "robot.geom.bin") == v2));
	BOOST_TEST((read_file(dir.path() / "a" / "robot.geom.bin.new") == v3));

	// Holding other bytes than asked: nothing is written or removed.
	BOOST_TEST(!resolver.commit_staged(staged, target, v2));
	BOOST_TEST(!resolver.remove_if_same(staged, v2));
	BOOST_TEST((read_file(dir.path() / "a" / "robot.geom.bin") == v2));
	BOOST_TEST(static_cast<bool>(resolver.remove_if_same(staged, v3)));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "robot.geom.bin.new"));
}

BOOST_FIXTURE_TEST_CASE(ACommittingReadOnAReadOnlyMountWritesNothing, two_layers)
{
	std::vector<res::binary_range> old_ranges;
	const auto old_file = encoded(k_positions, k_indices, old_ranges);
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions_v2, k_indices_v2, ranges);
	write_file(top / "a" / "robot.geom.desc", bytes_of(desc_text(ranges)));
	write_file(bin(), old_file);
	write_file(staged(), file);

	res::resource_system sys(tasks, read_only_table());
	const auto data = res::desc_binary::read(sys, desc, ranges, false, res::desc_binary::max_file_bytes,
		res::desc_binary_recovery::commit);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(data.from_staged);
	BOOST_TEST((read_file(bin()) == old_file));
	BOOST_TEST((read_file(staged()) == file));
}

BOOST_FIXTURE_TEST_CASE(ACommittingReadOfALimitedLayerWritesNothing, two_layers)
{
	res::resource_system sys(tasks, table());
	const auto first = save(sys, desc, k_positions, k_indices);
	const auto second = save_cut_before_commit(sys, desc, k_positions_v2, k_indices_v2);

	res::resource_system layer(sys);
	layer.limit_writes_to("memory://modules/game/");
	const auto data = res::desc_binary::read(layer, desc, second.ranges, false, res::desc_binary::max_file_bytes,
		res::desc_binary_recovery::commit);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	BOOST_TEST(data.from_staged);
	BOOST_TEST((read_file(bin()) == first.file));
	BOOST_TEST((read_file(staged()) == second.file));
}

BOOST_FIXTURE_TEST_CASE(OverSeveralRootsHashesAreAlwaysChecked, two_layers)
{
	std::vector<res::binary_range> ranges;
	auto file = encoded(k_positions, k_indices, ranges);
	file[static_cast<std::size_t>(ranges[0].offset)] ^= std::byte{ 0x01 };
	write_file(top / "a" / "robot.geom.desc", bytes_of(desc_text(ranges)));
	write_file(bin(), file);
	{
		res::resource_system sys(tasks, table());
		BOOST_TEST((res::desc_binary::read(sys, desc, ranges, false).error == res::desc_binary_error::hash));
	}
	res::resource_system single(tasks, single_root_table());
	BOOST_TEST(static_cast<bool>(res::desc_binary::read(single, desc, ranges, false)));
}

BOOST_FIXTURE_TEST_CASE(AFolderInPlaceOfTheBinaryFileIsAnIoError, two_layers)
{
	res::resource_system sys(tasks, table());
	std::vector<res::binary_range> ranges;
	encoded(k_positions, k_indices, ranges);
	fs::create_directories(bin());
	const auto data = res::desc_binary::read(sys, desc, ranges, true);
	BOOST_TEST((data.error == res::desc_binary_error::io));
}

BOOST_FIXTURE_TEST_CASE(AStagedFileThatDoesNotMatchTheDescIsNotUsed, two_layers)
{
	res::resource_system sys(tasks, table());
	std::vector<res::binary_range> ranges;
	auto file = encoded(k_positions, k_indices, ranges);
	auto other = file;
	other[static_cast<std::size_t>(ranges[1].offset)] ^= std::byte{ 0x01 };
	file[static_cast<std::size_t>(ranges[0].offset)] ^= std::byte{ 0x01 };
	write_file(top / "a" / "robot.geom.bin", file);
	write_file(top / "a" / "robot.geom.bin.new", other);

	const auto data = res::desc_binary::read(sys, desc, ranges, false);
	BOOST_TEST((data.error == res::desc_binary_error::hash));
	BOOST_TEST(fs::exists(top / "a" / "robot.geom.bin.new"));
}

BOOST_FIXTURE_TEST_CASE(ABinaryFileOfAnotherLayerIsNotPickedUp, two_layers)
{
	res::resource_system sys(tasks, table());
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	write_file(top / "a" / "robot.geom.desc", bytes_of(desc_text(ranges)));
	write_file(lower / "a" / "robot.geom.bin", file);

	const auto data = res::desc_binary::read(sys, desc, ranges, true);
	BOOST_TEST((data.error == res::desc_binary_error::missing));
	BOOST_TEST(data.message.find("robot.geom.bin") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(ADescOfALowerLayerReadsTheBinaryFileOfItsOwnLayer, two_layers)
{
	res::resource_system sys(tasks, table());
	std::vector<res::binary_range> ranges;
	const auto file = encoded(k_positions, k_indices, ranges);
	std::vector<res::binary_range> other_ranges;
	const auto other = encoded(k_positions_v2, k_indices_v2, other_ranges);
	write_file(lower / "a" / "robot.geom.desc", bytes_of(desc_text(ranges)));
	write_file(lower / "a" / "robot.geom.bin", file);
	write_file(top / "a" / "robot.geom.bin", other);

	const auto data = res::desc_binary::read(sys, desc, ranges, true);
	BOOST_REQUIRE_MESSAGE(data, data.message);
	std::vector<float> positions;
	BOOST_TEST(res::copy_binary_values(data.ranges[0], positions));
	BOOST_TEST((positions == k_positions));
}

BOOST_AUTO_TEST_SUITE_END()
