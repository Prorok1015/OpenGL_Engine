#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_decode_bundle.h"
#include <boost/json/object.hpp>
#include <cstdint>
#include <fstream>
#include <string>

// The bundle the decoder process writes and the editor reads back (#647): the reader treats the file as untrusted
// input, so a size the file cannot hold is refused before anything is allocated.

namespace {

namespace fs = std::filesystem;

void put_u32(std::string& out, std::uint32_t value)
{
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void put_u64(std::string& out, std::uint64_t value)
{
	for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

std::string raw_header(std::uint64_t manifest_size, std::uint64_t blob_count, std::uint32_t version = 1, const char* magic = "SNKB")
{
	std::string out(magic, 4);
	put_u32(out, version);
	put_u64(out, manifest_size);
	put_u64(out, blob_count);
	return out;
}

std::string raw_blob_head(const std::string& tag, std::uint64_t size)
{
	std::string out;
	put_u32(out, static_cast<std::uint32_t>(tag.size()));
	out += tag;
	put_u64(out, size);
	return out;
}

struct bundle_fixture
{
	fs::path write_raw(const std::string& bytes)
	{
		const fs::path file = dir.path() / "bundle.bin";
		std::ofstream(file, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		return file;
	}

	std::string read_error(const std::string& bytes, const edt::bundle_limits& limits = {})
	{
		const edt::bundle_read_result result = edt::read_bundle(write_raw(bytes), limits);
		BOOST_CHECK(!result.ok());
		return result.error;
	}

	test_util::temp_dir dir{ "decode_bundle" };
};

std::vector<std::byte> bytes_of(const std::string& text)
{
	std::vector<std::byte> out(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
	return out;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtDecodeBundleTests, bundle_fixture)

BOOST_AUTO_TEST_CASE(ABundle_RoundTrips_ManifestAndResources)
{
	edt::decode_bundle bundle;
	bundle.manifest["prefab"] = "memory://robot/robot.prefab.desc";
	bundle.manifest["skin_weights"] = boost::json::array{ 1, 2, 3 };
	bundle.blobs.push_back({ "memory://robot/robot.prefab.desc", bytes_of("{\"a\":1}") });
	bundle.blobs.push_back({ "memory://robot/tex/albedo.png", {} });
	std::string error;
	const fs::path file = dir.path() / "round.bin";
	BOOST_REQUIRE_MESSAGE(edt::write_bundle(file, bundle, error), error);

	const edt::bundle_read_result back = edt::read_bundle(file);
	BOOST_REQUIRE_MESSAGE(back.ok(), back.error);
	BOOST_CHECK_EQUAL(boost::json::serialize(back.bundle->manifest), boost::json::serialize(bundle.manifest));
	BOOST_REQUIRE_EQUAL(back.bundle->blobs.size(), 2u);
	BOOST_CHECK_EQUAL(back.bundle->blobs[0].tag, "memory://robot/robot.prefab.desc");
	BOOST_CHECK(back.bundle->blobs[0].bytes == bundle.blobs[0].bytes);
	BOOST_CHECK(back.bundle->blobs[1].bytes.empty());
}

BOOST_AUTO_TEST_CASE(ADeclaredBlobOfFourGiB_IsRefusedWithoutBeingAllocated)
{
	const std::string manifest = "{}";
	std::string bytes = raw_header(manifest.size(), 1) + manifest + raw_blob_head("memory://x/y.bin", 4ull * 1024 * 1024 * 1024);
	BOOST_CHECK(read_error(bytes).find("too big") != std::string::npos);

	// Even when the limits would allow it, the file does not hold it: nothing is allocated for what is declared.
	edt::bundle_limits wide;
	wide.max_blob_bytes = 8ull * 1024 * 1024 * 1024;
	wide.max_total_bytes = 8ull * 1024 * 1024 * 1024;
	BOOST_CHECK(read_error(bytes, wide).find("bigger than the file holds") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TooManyResourcesOrATooBigManifest_AreRefusedByTheirDeclaredCounts)
{
	BOOST_CHECK(read_error(raw_header(2, 1ull << 40) + "{}").find("too many") != std::string::npos);
	BOOST_CHECK(read_error(raw_header(1ull << 40, 0)).find("manifest") != std::string::npos);
	edt::bundle_limits tight;
	tight.max_blobs = 1;
	BOOST_CHECK(read_error(raw_header(2, 2) + "{}", tight).find("too many") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AManifestThatIsNotAnObjectOrIsTooDeep_IsRefused)
{
	BOOST_CHECK(read_error(raw_header(2, 0) + "[]").find("object") != std::string::npos);
	const std::string deep = std::string(200, '[') + std::string(200, ']');
	BOOST_CHECK(read_error(raw_header(deep.size(), 0) + deep).find("JSON") != std::string::npos);
	BOOST_CHECK(read_error(raw_header(3, 0) + "{,}").find("JSON") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AWrongMagicOrVersion_IsRefused)
{
	BOOST_CHECK(read_error(raw_header(2, 0, 1, "XXXX") + "{}").find("not a decoder bundle") != std::string::npos);
	BOOST_CHECK(read_error(raw_header(2, 0, 2) + "{}").find("version") != std::string::npos);
	BOOST_CHECK(read_error("SN").find("not a decoder bundle") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ATruncatedBundleOrOneWithBytesAfterItsEnd_IsRefused)
{
	const std::string manifest = "{}";
	const std::string good = raw_header(manifest.size(), 1) + manifest + raw_blob_head("memory://x/y.bin", 4) + "abcd";
	const edt::bundle_read_result ok = edt::read_bundle(write_raw(good));
	BOOST_CHECK_MESSAGE(ok.ok(), ok.error);

	BOOST_CHECK(!read_error(good.substr(0, good.size() - 1)).empty());
	BOOST_CHECK(read_error(good + "x").find("after its end") != std::string::npos);
	BOOST_CHECK(!read_error(good.substr(0, 10)).empty());
}

BOOST_AUTO_TEST_CASE(ATagThatIsRepeatedOrNotAllowed_IsRefused)
{
	const std::string manifest = "{}";
	const std::string twice = raw_header(manifest.size(), 2) + manifest + raw_blob_head("memory://x/y.bin", 0) + raw_blob_head("memory://x/y.bin", 0);
	BOOST_CHECK(read_error(twice).find("twice") != std::string::npos);

	for (const char* tag : { "res://x/y.bin", "memory://", "memory:///etc/passwd", "memory://x/../y", "memory://x//y", "memory://x\\y", "memory://x/y:z" }) {
		BOOST_TEST_CONTEXT(tag) {
			const std::string one = raw_header(manifest.size(), 1) + manifest + raw_blob_head(tag, 0);
			BOOST_CHECK(read_error(one).find("tag") != std::string::npos);
			edt::decode_bundle bundle;
			bundle.blobs.push_back({ tag, {} });
			std::string error;
			BOOST_CHECK(!edt::write_bundle(dir.path() / "refused.bin", bundle, error));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheTotalOfAllResources_IsLimitedToo)
{
	edt::decode_bundle bundle;
	bundle.blobs.push_back({ "memory://a/one.bin", std::vector<std::byte>(600) });
	bundle.blobs.push_back({ "memory://a/two.bin", std::vector<std::byte>(600) });
	std::string error;
	const fs::path file = dir.path() / "two.bin";
	BOOST_REQUIRE_MESSAGE(edt::write_bundle(file, bundle, error), error);
	edt::bundle_limits limits;
	limits.max_total_bytes = 1000;
	const edt::bundle_read_result result = edt::read_bundle(file, limits);
	BOOST_CHECK(!result.ok());
	BOOST_CHECK(result.error.find("together") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
