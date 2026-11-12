#include <boost/test/unit_test.hpp>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

constexpr std::string_view full_meta =
	"{\n"
	"    \"__type\" : \"resource_meta\",\n"
	"    \"__version\" : 1,\n"
	"    \"__guid\" : \"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f\",\n"
	"    \"importer\" : \"texture\",\n"
	"    \"settings\" : {\n"
	"        \"srgb\" : true,\n"
	"        \"mips\" : {\n"
	"            \"count\" : 4,\n"
	"            \"filter\" : \"box\"\n"
	"        },\n"
	"        \"scale\" : 0.5\n"
	"    },\n"
	"    \"flags\" : [\"no_pack\", \"editor_only\"]\n"
	"}\n";

constexpr std::string_view bare_meta =
	"{\n"
	"    \"__type\" : \"resource_meta\",\n"
	"    \"__version\" : 1,\n"
	"    \"__guid\" : \"00000000-0000-4000-8000-000000000001\",\n"
	"    \"importer\" : \"\",\n"
	"    \"settings\" : {},\n"
	"    \"flags\" : []\n"
	"}\n";

std::string replaced(std::string_view text, std::string_view from, std::string_view to)
{
	std::string out{ text };
	const auto at = out.find(from);
	BOOST_REQUIRE(at != std::string::npos);
	out.replace(at, from.size(), to);
	return out;
}

void require_error(std::string_view text, std::string_view field_path,
	res::asset_meta_error_kind kind = res::asset_meta_error_kind::malformed)
{
	const auto result = res::read_asset_meta(text);
	BOOST_REQUIRE(!result);
	BOOST_TEST(result.error.field_path == field_path);
	BOOST_TEST((result.error.kind == kind));
	BOOST_TEST(!result.error.message.empty());
}

bool is_v4(const res::asset_id& id)
{
	const std::string text = res::to_text(id);
	return text[14] == '4' && std::string_view{ "89ab" }.find(text[19]) != std::string_view::npos;
}

}

BOOST_AUTO_TEST_SUITE(ResAssetId)

BOOST_AUTO_TEST_CASE(text_round_trips_without_loss)
{
	const auto id = res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	BOOST_REQUIRE(id.has_value());
	BOOST_TEST(id->hi == 0x3f2a9c1e7b4d4e8aull);
	BOOST_TEST(id->lo == 0x9c0f1a2b3c4d5e6full);
	BOOST_TEST(res::to_text(*id) == "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");

	const res::asset_id extremes{ 0xffffffffffffffffull, 0x0123456789abcdefull };
	BOOST_TEST((res::asset_id_from_text(res::to_text(extremes)) == extremes));
}

BOOST_AUTO_TEST_CASE(upper_case_reads_and_writes_lower_case)
{
	const auto id = res::asset_id_from_text("3F2A9C1E-7B4D-4E8A-9C0F-1A2B3C4D5E6F");
	BOOST_REQUIRE(id.has_value());
	BOOST_TEST(res::to_text(*id) == "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
}

BOOST_AUTO_TEST_CASE(malformed_text_is_nullopt)
{
	BOOST_TEST(!res::asset_id_from_text(""));
	BOOST_TEST(!res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6"));    // short
	BOOST_TEST(!res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f0"));  // long
	BOOST_TEST(!res::asset_id_from_text("3f2a9c1e7-b4d-4e8a-9c0f-1a2b3c4d5e6f"));   // dash moved
	BOOST_TEST(!res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6g"));   // not hex
	BOOST_TEST(!res::asset_id_from_text("{3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6}"));  // braces
}

BOOST_AUTO_TEST_CASE(generated_ids_are_v4_and_distinct)
{
	std::unordered_set<res::asset_id> seen;
	for (int i = 0; i < 64; ++i) {
		const auto id = res::generate_asset_id();
		BOOST_TEST(is_v4(id));
		BOOST_TEST(!id.is_nil());
		seen.insert(id);
	}
	BOOST_TEST(seen.size() == 64u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ResAssetMeta)

BOOST_AUTO_TEST_CASE(read_then_write_gives_the_same_bytes)
{
	for (const std::string_view text : { full_meta, bare_meta }) {
		const auto result = res::read_asset_meta(text);
		BOOST_REQUIRE_MESSAGE(result, result.error.field_path + ": " + result.error.message);
		BOOST_TEST(res::write_asset_meta(*result.meta) == text);
	}
}

BOOST_AUTO_TEST_CASE(read_fills_every_field)
{
	const auto result = res::read_asset_meta(full_meta);
	BOOST_REQUIRE(result);
	const res::asset_meta& meta = *result.meta;
	BOOST_TEST(res::to_text(meta.guid) == "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	BOOST_TEST(meta.importer == "texture");
	BOOST_TEST(meta.settings.at("srgb").as_bool());
	BOOST_TEST(meta.settings.at("mips").at("count").to_number<int>() == 4);
	BOOST_TEST(meta.flags.size() == 2u);
	BOOST_TEST(meta.flags[1] == "editor_only");
}

BOOST_AUTO_TEST_CASE(write_puts_keys_in_the_fixed_order)
{
	res::asset_meta meta;
	meta.flags = { "no_pack", "editor_only" };
	meta.settings["srgb"] = true;
	meta.settings["mips"] = json::object{ { "count", 4 }, { "filter", "box" } };
	meta.settings["scale"] = 0.5;
	meta.importer = "texture";
	meta.guid = *res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	BOOST_TEST(res::write_asset_meta(meta) == full_meta);
}

BOOST_AUTO_TEST_CASE(a_bad_guid_names_the_field)
{
	require_error(replaced(full_meta, "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "not-a-guid"), "__guid");
	require_error(replaced(full_meta, "\"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f\"", "42"), "__guid");
	require_error(replaced(full_meta, "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "00000000-0000-0000-0000-000000000000"), "__guid");
}

BOOST_AUTO_TEST_CASE(a_bad_version_names_the_field)
{
	require_error(replaced(full_meta, "\"__version\" : 1", "\"__version\" : \"1\""), "__version");
	require_error(replaced(full_meta, "\"__version\" : 1", "\"__version\" : 0"), "__version");
	require_error(replaced(full_meta, "\"__version\" : 1", "\"__version\" : 1.5"), "__version");
	require_error(replaced(full_meta, "    \"__version\" : 1,\n", ""), "__version");
}

BOOST_AUTO_TEST_CASE(a_newer_version_is_its_own_refusal)
{
	// A newer file may carry keys this engine does not know; the version is what refuses it, not the key.
	const std::string newer = replaced(replaced(full_meta, "\"__version\" : 1", "\"__version\" : 2"),
		"    \"flags\"", "    \"source_hash\" : \"x\",\n    \"flags\"");
	require_error(newer, "__version", res::asset_meta_error_kind::newer_than_engine);
}

BOOST_AUTO_TEST_CASE(other_fields_are_checked_with_their_path)
{
	require_error(replaced(full_meta, "\"resource_meta\"", "\"material\""), "__type");
	require_error(replaced(full_meta, "\"importer\" : \"texture\"", "\"importer\" : 3"), "importer");
	require_error(replaced(full_meta, "    \"importer\" : \"texture\",\n", ""), "importer");
	require_error(replaced(bare_meta, "\"settings\" : {}", "\"settings\" : []"), "settings");
	require_error(replaced(full_meta, "[\"no_pack\", \"editor_only\"]", "\"no_pack\""), "flags");
	require_error(replaced(full_meta, "[\"no_pack\", \"editor_only\"]", "[\"no_pack\", 7]"), "flags[1]");
	require_error(replaced(full_meta, "    \"flags\"", "    \"extra\" : null,\n    \"flags\""), "extra");
}

BOOST_AUTO_TEST_CASE(a_document_that_is_not_a_meta_object_fails_at_the_root)
{
	require_error("not json", "");
	require_error("[1, 2]", "");
}

BOOST_AUTO_TEST_SUITE_END()
