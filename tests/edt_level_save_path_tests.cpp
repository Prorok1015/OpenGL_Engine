#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>

// Saving a level writes through the resource system, never to a path of its own (#394): a Save As path that leaves
// the resource root, or a level file that is a symlink, is refused and nothing is written outside the root. The save
// waits for the write (store_sync), so these checks need no polling, and a failed write leaves the document dirty.

namespace {

namespace fs = std::filesystem;

// The temp files of the writes in `folder` (".<name>.<random>.tmp"): none may be left behind.
std::size_t temp_files_in(const fs::path& folder)
{
	std::size_t count = 0;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(folder, ec)) {
		if (entry.path().extension() == ".tmp") ++count;
	}
	return count;
}

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct level_save_fixture : test_util::scene_env
{
	level_save_fixture()
	{
		fs::create_directories(content);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ content });

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		// save_level_as only needs a loaded level to save; the worlds may be empty.
		state.editor_tag = tag_of("level.desc");
	}

	// The resolver's io thread and watcher go before the temp folder they work in.
	~level_save_fixture() { rs->unregistrate_resolver(res::tag::default_protocol()); }

	test_util::temp_dir dir{ "level_save" };
	const fs::path content = dir.path() / "content";
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::scene_document state;
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtLevelSavePathTests, level_save_fixture)

BOOST_AUTO_TEST_CASE(SaveAs_WritesTheLevelInsideTheRoot)
{
	state.commit("edit", edt::commit_mode::state_only);
	BOOST_REQUIRE(level_ctrl.save_level_as("levels/new.desc"));

	BOOST_TEST(fs::exists(content / "levels" / "new.desc"));
	BOOST_TEST(!state.is_dirty());
	BOOST_TEST((state.level_tag == res::tag::make("levels/new.desc")));
	BOOST_TEST(read_file(content / "levels" / "new.desc").find("\"worlds\"") != std::string::npos);
	BOOST_TEST(temp_files_in(content / "levels") == 0u);
}

BOOST_AUTO_TEST_CASE(SaveAs_AFailedWriteKeepsTheDocumentDirty)
{
	// A folder where the level file should be: the write itself succeeds, replacing the target cannot.
	dir.write("content/levels/busy.desc/keep.txt", "keep");
	state.commit("edit", edt::commit_mode::state_only);

	BOOST_TEST(!level_ctrl.save_level_as("levels/busy.desc"));

	BOOST_TEST(state.is_dirty());
	BOOST_TEST(!state.level_tag.is_valid());
	BOOST_TEST(read_file(content / "levels" / "busy.desc" / "keep.txt") == "keep");
	BOOST_TEST(temp_files_in(content / "levels") == 0u);
}

BOOST_AUTO_TEST_CASE(SaveAs_RefusesAPathWithAProtocol)
{
	BOOST_TEST(!level_ctrl.save_level_as("memory://levels/x.desc"));
	BOOST_TEST(!state.level_tag.is_valid());
}

BOOST_AUTO_TEST_CASE(SaveAs_RefusesAPathThatLeavesTheRoot)
{
	const fs::path absolute = dir.path() / "absolute.desc";
	for (const std::string& path : { std::string{ "../escape.desc" }, std::string{ "levels/../../escape.desc" },
			absolute.generic_string() }) {
		BOOST_TEST_INFO("path: " << path);
		BOOST_TEST(!level_ctrl.save_level_as(path));
		BOOST_TEST(!state.level_tag.is_valid());
	}
	BOOST_TEST(!fs::exists(dir.path() / "escape.desc"));
	BOOST_TEST(!fs::exists(absolute));
}

BOOST_AUTO_TEST_CASE(SaveAs_RefusesALevelFileThatIsASymlink)
{
	dir.write("outside/target.desc", "original");
	fs::create_directories(content / "levels");
	std::error_code ec;
	fs::create_symlink(dir.path() / "outside" / "target.desc", content / "levels" / "link.desc", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	BOOST_TEST(!level_ctrl.save_level_as("levels/link.desc"));
	BOOST_TEST(read_file(dir.path() / "outside" / "target.desc") == "original");
}

BOOST_AUTO_TEST_SUITE_END()
