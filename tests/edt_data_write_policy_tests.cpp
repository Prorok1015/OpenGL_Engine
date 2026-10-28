#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include <snk/resource/resources/res_resource_text.h>
#include <filesystem>
#include <string>
#include <vector>

// What edt:// may store into (#164): by default only edt://settings/**, loads are never restricted.

namespace {

struct edt_write_fixture : test_util::ram_res_env
{
	test_util::temp_dir dir{ "edt_write_policy" };
	std::filesystem::path data = dir.path() / "data";
	std::filesystem::path user = dir.path() / "user";

	edt_write_fixture() { dir.write("data/icons/logo.txt", "logo-bytes"); }

	// Joins the vfs io threads before the scratch directory goes away.
	~edt_write_fixture() { rs.reset(); }
};

}

BOOST_FIXTURE_TEST_SUITE(EdtWritePolicy, edt_write_fixture)

BOOST_AUTO_TEST_CASE(Store_OutsideTheDefaultPatterns_IsRefusedAndTheShippedFileStays)
{
	edt::register_data_protocol(*rs, data, user);

	BOOST_TEST(!rs->store(res::tag{ "edt://icons/logo.txt" }, test_util::to_bytes("overwritten")));
	BOOST_TEST(!rs->store(res::tag{ "edt://settings_evil/x.txt" }, test_util::to_bytes("x")));
	BOOST_TEST(!std::filesystem::exists(user / "icons"));

	auto logo = rs->require_sync<res::text_resource>(res::tag{ "edt://icons/logo.txt" });
	pump();
	BOOST_REQUIRE(!logo.has_error());
	BOOST_TEST(logo.get_sync()->str() == "logo-bytes");
}

BOOST_AUTO_TEST_CASE(Store_InsideTheDefaultPatterns_IsAccepted)
{
	edt::register_data_protocol(*rs, data, user);

	BOOST_TEST(rs->store(res::tag{ "edt://settings/layout.ini" }, test_util::to_bytes("x")));
	BOOST_TEST(rs->store(res::tag{ "edt://settings/profiles/a.json" }, test_util::to_bytes("x")));
}

BOOST_AUTO_TEST_CASE(Store_WithAnEmptyPatternList_RefusesEverything)
{
	edt::register_data_protocol(*rs, data, user, {});

	BOOST_TEST(!rs->store(res::tag{ "edt://settings/layout.ini" }, test_util::to_bytes("x")));
}

BOOST_AUTO_TEST_CASE(DefaultWritePatterns_AreTheSettingsFolderOnly)
{
	BOOST_TEST((edt::default_write_patterns() == std::vector<std::string>{ "edt://settings/**" }));
}

BOOST_AUTO_TEST_SUITE_END()
