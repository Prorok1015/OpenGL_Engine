#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_entry.h>
#include <snk/application/app_module_source.h>
#include <snk/editor/edt_editor_extension.h>
#include <snk/editor/edt_editor_layout_key.h>
#include <filesystem>
#include <string>

#if defined(SNK_TEST_EDITOR_MODULE_DLL_PATH)

// An editor module built as its own library reports the editor layout key of this host across the binary boundary
// (#969): the key a module computes from its own view of the editor types equals the host's. Shared engine only.

BOOST_AUTO_TEST_SUITE(EdtEditorModuleDllTests)

BOOST_AUTO_TEST_CASE(AnEditorModuleLibrary_ExportsTheHostsEditorLayoutKey_AndPassesTheHostsChecks)
{
	const std::filesystem::path path = std::filesystem::absolute(SNK_TEST_EDITOR_MODULE_DLL_PATH);
	app::modules::native_library_source source(path, path.parent_path());
	const std::string problem = source.open();
	BOOST_REQUIRE_MESSAGE(problem.empty(), problem);

	const app::modules::module_entry& entry = source.entry();
	BOOST_REQUIRE(entry.editor_layout_key != nullptr);
	BOOST_TEST(entry.editor_layout_key() == edt::editor_layout_key());
	BOOST_TEST(edt::check_editor_api_version(entry.editor_api_version, entry.editor_layout_key).empty());
	BOOST_TEST(app::modules::check_library_module(entry).empty());

	source.close();
	BOOST_TEST(!source.is_open());
}

BOOST_AUTO_TEST_SUITE_END()

#endif
