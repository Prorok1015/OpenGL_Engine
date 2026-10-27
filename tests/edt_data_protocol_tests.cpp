#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include <snk/resource/resources/res_resource_text.h>
#include <filesystem>

// edt:// (#161): the protocol for editor-only resources, mounted over Editor/data.

namespace {

struct edt_data_fixture : test_util::ram_res_env
{
	test_util::temp_dir dir{ "edt_data" };
	std::filesystem::path data = dir.path() / "data";

	edt_data_fixture()
	{
		dir.write("data/icons/logo.txt", "logo-bytes");
		dir.write("secret.txt", "outside the data directory");
	}

	res::res_handle<res::text_resource> load(std::string_view tag)
	{
		auto handle = rs->require_sync<res::text_resource>(res::tag{ tag });
		pump();
		return handle;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDataProtocol, edt_data_fixture)

BOOST_AUTO_TEST_CASE(Register_ExistingDirectory_ResolvesFilesUnderEdt)
{
	BOOST_TEST(edt::register_data_protocol(*rs, data));

	BOOST_TEST(rs->exists(res::tag{ "edt://icons/logo.txt" }));
	auto handle = load("edt://icons/logo.txt");
	BOOST_REQUIRE(!handle.has_error());
	BOOST_TEST(handle.get_sync()->str() == "logo-bytes");
}

BOOST_AUTO_TEST_CASE(Register_ExistingDirectory_DoesNotServeResProtocolFiles)
{
	edt::register_data_protocol(*rs, data);

	BOOST_TEST(!rs->exists(res::tag{ "res://icons/logo.txt" }));
}

BOOST_AUTO_TEST_CASE(Resolve_ParentDirectoryEscape_IsRejected)
{
	edt::register_data_protocol(*rs, data);

	BOOST_TEST(!rs->exists(res::tag{ "edt://../secret.txt" }));
	BOOST_TEST(load("edt://../secret.txt").has_error());
	BOOST_TEST(load("edt://icons/../../secret.txt").has_error());
}

BOOST_AUTO_TEST_CASE(Register_MissingDirectory_ReturnsFalseAndLoadsFail)
{
	BOOST_TEST(!edt::register_data_protocol(*rs, dir.path() / "no_such_data"));

	BOOST_TEST(!rs->exists(res::tag{ "edt://icons/logo.txt" }));
	BOOST_TEST(load("edt://icons/logo.txt").has_error());
}

BOOST_AUTO_TEST_SUITE_END()
