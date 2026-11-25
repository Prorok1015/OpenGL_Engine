#include <boost/test/unit_test.hpp>
#include "edt_model_import_form.h"
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct form_fixture
{
	edt::model_import_settings_form form;
	std::vector<std::pair<std::string, edt::model_import_settings>> imported;

	edt::model_import_settings_form::import_fn recorder()
	{
		return [this](const fs::path& source, const edt::model_import_settings& settings) {
			imported.emplace_back(source.generic_string(), settings);
		};
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtModelImportForm, form_fixture)

BOOST_AUTO_TEST_CASE(Open_ShowsTheModel_WithTheDefaults)
{
	BOOST_CHECK(!form.is_open());
	BOOST_REQUIRE(form.open("/models/robot.glb", recorder()));
	BOOST_CHECK(form.is_open());
	BOOST_TEST(form.shown_name() == "robot.glb");
	BOOST_CHECK(form.settings() == edt::model_import_settings{});
	BOOST_CHECK(imported.empty());
}

BOOST_AUTO_TEST_CASE(Submit_ImportsOnce_WithTheSettingsChosen)
{
	BOOST_REQUIRE(form.open("/models/robot.glb", recorder()));
	form.set_normals(edt::model_normals::flat);
	form.set_tangents(false);
	form.submit();
	BOOST_CHECK(!form.is_open());
	BOOST_REQUIRE_EQUAL(imported.size(), 1u);
	BOOST_TEST(imported[0].first == "/models/robot.glb");
	BOOST_CHECK(imported[0].second.normals == edt::model_normals::flat);
	BOOST_CHECK(!imported[0].second.tangents);
	form.submit();
	BOOST_CHECK_EQUAL(imported.size(), 1u);
}

BOOST_AUTO_TEST_CASE(Cancel_ImportsNothing_AndTheNextOpenStartsFromTheDefaults)
{
	BOOST_REQUIRE(form.open("/models/robot.glb", recorder()));
	form.set_normals(edt::model_normals::flat);
	form.cancel();
	BOOST_CHECK(!form.is_open());
	form.submit();
	BOOST_CHECK(imported.empty());

	BOOST_REQUIRE(form.open("/models/other.glb", recorder()));
	BOOST_CHECK(form.settings() == edt::model_import_settings{});
}

BOOST_AUTO_TEST_CASE(ASecondModel_WhileOpen_IsRefused)
{
	BOOST_REQUIRE(form.open("/models/robot.glb", recorder()));
	BOOST_CHECK(!form.open("/models/other.glb", recorder()));
	BOOST_TEST(form.shown_name() == "robot.glb");
	BOOST_CHECK(!edt::model_import_settings_form{}.open("/models/robot.glb", nullptr));
}

BOOST_AUTO_TEST_CASE(TheImport_MayOpenTheWindowAgain)
{
	BOOST_REQUIRE(form.open("/models/robot.glb", [this](const fs::path&, const edt::model_import_settings&) {
		BOOST_CHECK(form.open("/models/next.glb", recorder()));
	}));
	form.submit();
	BOOST_CHECK(form.is_open());
	BOOST_TEST(form.shown_name() == "next.glb");
}

BOOST_AUTO_TEST_CASE(AFileNameWithControlCharacters_IsShownEscaped)
{
	BOOST_REQUIRE(form.open(fs::path("/models") / "a\x1b[31m.glb", recorder()));
	BOOST_CHECK(form.shown_name().find('\x1b') == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
