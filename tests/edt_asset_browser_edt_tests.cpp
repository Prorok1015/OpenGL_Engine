#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_layers.h"
#include "edt_data_protocol.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// The asset browser shows res:// only (#165): editor-only resources behind edt:// never reach its layers.

#ifndef SNK_EDITOR_RES_DIR
#error "SNK_EDITOR_RES_DIR must point at Editor/res"
#endif
#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif
#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

namespace fs = std::filesystem;

namespace {

bool lists(const std::vector<edt::layer_entry>& entries, const std::string& name)
{
	return std::ranges::any_of(entries, [&](const edt::layer_entry& e) { return e.name == name; });
}

}

BOOST_AUTO_TEST_SUITE(EdtAssetBrowserHidesEdt)

BOOST_AUTO_TEST_CASE(ShippedResLayers_ListNoEditorOnlyFolders)
{
	const std::vector<fs::path> layers{ SNK_CORE_RES_DIR, SNK_EDITOR_RES_DIR };

	const auto root = edt::list_merged_directory(layers, {});

	BOOST_TEST(lists(root, "shaders"));
	BOOST_TEST(lists(root, "levels"));
	for (const char* editor_only : { "icons", "templates", "settings", "data" }) {
		BOOST_TEST(!lists(root, editor_only), editor_only);
	}
}

BOOST_AUTO_TEST_CASE(ShippedEditorData_IsNotReachableThroughTheResLayers)
{
	const std::vector<fs::path> layers{ SNK_CORE_RES_DIR, SNK_EDITOR_RES_DIR };

	BOOST_TEST(edt::list_merged_directory(layers, "icons").empty());
	BOOST_TEST(edt::list_merged_directory(layers, "templates").empty());
	BOOST_TEST(!edt::list_merged_directory({ SNK_EDITOR_DATA_DIR }, "icons").empty());
}

BOOST_FIXTURE_TEST_CASE(RegisteringEdt_DoesNotAddALayerToTheBrowser, test_util::ram_res_env)
{
	const auto layers_before = res::resource_system::get_resource_layers();

	test_util::temp_dir data("asset_browser_edt_data");
	BOOST_REQUIRE(edt::register_data_protocol(*rs, data.path()));

	const auto layers_after = res::resource_system::get_resource_layers();
	BOOST_TEST((layers_before == layers_after));
	for (const fs::path& layer : layers_after) {
		BOOST_TEST(layer != data.path(), layer);
	}
}

BOOST_AUTO_TEST_SUITE_END()
