#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include <snk/resource/resources/res_resource_text.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#ifndef SNK_TEST_PROJECT_DIR
#error "SNK_TEST_PROJECT_DIR must point at projects/sandbox"
#endif
#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

// edt:// (#161): the protocol for editor-only resources, mounted over Editor/data.

namespace {

struct edt_data_fixture : test_util::ram_res_env
{
	test_util::temp_dir dir{ "edt_data" };
	std::filesystem::path data = dir.path() / "data";
	std::filesystem::path user = dir.path() / "user";

	edt_data_fixture()
	{
		dir.write("data/icons/logo.txt", "logo-bytes");
		dir.write("secret.txt", "outside the data directory");
	}

	// Joins the vfs io threads before the scratch directory goes away.
	~edt_data_fixture() { rs.reset(); }

	static bool wait_for_text(const std::filesystem::path& file, const std::string& text)
	{
		for (int i = 0; i < 500; ++i) {
			std::ifstream in(file, std::ios::binary);
			const std::string content{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			if (in.is_open() && content == text) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
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
	BOOST_TEST(edt::register_data_protocol(*rs, data, user));

	BOOST_TEST(rs->exists(res::tag{ "edt://icons/logo.txt" }));
	auto handle = load("edt://icons/logo.txt");
	BOOST_REQUIRE(!handle.has_error());
	BOOST_TEST(handle.get_sync()->str() == "logo-bytes");
}

BOOST_AUTO_TEST_CASE(Register_ExistingDirectory_DoesNotServeResProtocolFiles)
{
	edt::register_data_protocol(*rs, data, user);

	BOOST_TEST(!rs->exists(res::tag{ "res://icons/logo.txt" }));
}

BOOST_AUTO_TEST_CASE(Resolve_ParentDirectoryEscape_IsRejected)
{
	edt::register_data_protocol(*rs, data, user);

	BOOST_TEST(!rs->exists(res::tag{ "edt://../secret.txt" }));
	BOOST_TEST(load("edt://../secret.txt").has_error());
	BOOST_TEST(load("edt://icons/../../secret.txt").has_error());
}

BOOST_AUTO_TEST_CASE(Register_MissingDirectory_ReturnsFalseAndLoadsFail)
{
	BOOST_TEST(!edt::register_data_protocol(*rs, dir.path() / "no_such_data", user));

	BOOST_TEST(!rs->exists(res::tag{ "edt://icons/logo.txt" }));
	BOOST_TEST(load("edt://icons/logo.txt").has_error());
}

BOOST_AUTO_TEST_CASE(ShippedEditorData_HoldsIconsAndTemplates_ResRootDoesNot)
{
	namespace fs = std::filesystem;
	for (const char* file : { "icons/editor_engine_logo.png", "icons/engine_logo.png", "icons/engine_logo_small.png",
	                          "templates/level.desc", "templates/world.desc", "templates/prefab_preview.desc" }) {
		BOOST_TEST(fs::is_regular_file(fs::path{ SNK_EDITOR_DATA_DIR } / file), file);
	}
	for (const char* dir : { "icons", "templates" }) {
		BOOST_TEST(!fs::exists((fs::path{ SNK_TEST_PROJECT_DIR } / "res") / dir), dir);
	}
}

BOOST_AUTO_TEST_CASE(Store_Settings_GoesToTheUserRootAndNeverIntoTheDataDirectory)
{
	edt::register_data_protocol(*rs, data, user);

	BOOST_REQUIRE(rs->store(res::tag{ "edt://settings/layout.ini" }, test_util::to_bytes("layout")));

	BOOST_TEST(wait_for_text(user / "settings" / "layout.ini", "layout"));
	BOOST_TEST(!std::filesystem::exists(data / "settings"));
}

BOOST_AUTO_TEST_CASE(Store_OutsideTheSettings_IsRefusedAndTheShippedFileStays)
{
	edt::register_data_protocol(*rs, data, user);

	BOOST_TEST(!rs->store(res::tag{ "edt://icons/logo.txt" }, test_util::to_bytes("overwritten")));
	BOOST_TEST(!std::filesystem::exists(user / "icons"));
	BOOST_TEST(load("edt://icons/logo.txt").get_sync()->str() == "logo-bytes");
}

BOOST_AUTO_TEST_CASE(Load_UserFileShadowsTheShippedOne)
{
	edt::register_data_protocol(*rs, data, user);
	BOOST_REQUIRE(load("edt://icons/logo.txt").get_sync()->str() == "logo-bytes");

	dir.write("user/icons/logo.txt", "user-logo");

	rs->signal_changed(res::tag{ "edt://icons/logo.txt" });
	BOOST_TEST(load("edt://icons/logo.txt").get_sync()->str() == "user-logo");
}

BOOST_AUTO_TEST_CASE(DefaultUserDataDir_NamesTheEditorFolderOfTheProduct)
{
	const auto path = edt::default_user_data_dir();

	BOOST_TEST(!path.empty());
#ifdef _WIN32
	BOOST_TEST(path.filename() == "Editor");
#else
	BOOST_TEST(path.filename() == "editor");
#endif
}

BOOST_AUTO_TEST_SUITE_END()
