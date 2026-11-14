#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include "edt_file_menu.h"
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// res:// as a project mounts it (its mount moved onto a lower layer and the project's content), so the save can read the
// file it replaces in whichever layer res:// reads it from.
struct level_guid_fixture : level_save_fixture
{
	level_guid_fixture()
	{
		fs::create_directories(lower);
		BOOST_REQUIRE(rs->remount(res::tag::default_protocol(), { lower, content }));
	}

	void write(const std::string& relative, const std::string& text, const fs::path& layer = {})
	{
		const fs::path file = (layer.empty() ? content : layer) / relative;
		fs::create_directories(file.parent_path());
		std::ofstream(file, std::ios::binary) << text;
	}

	const fs::path lower = dir.path() / "lower";

	boost::json::object saved(const std::string& relative) { return boost::json::parse(read_file(content / relative)).as_object(); }
};

// Path <-> ID both ways, as the editor's asset database answers.
struct ids_index final : res::asset_index
{
	std::map<std::string, res::asset_id, std::less<>> ids;
	std::map<res::asset_id, res::tag> paths;

	void hold(const res::asset_id& id, std::string_view path)
	{
		ids.emplace(std::string(path), id);
		paths.insert_or_assign(id, res::tag{ std::string(path) });
	}
	std::optional<res::asset_id> id_of(const res::tag& tag) const override
	{
		const auto found = ids.find(tag.view());
		return found == ids.end() ? std::nullopt : std::optional{ found->second };
	}
	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		const auto found = paths.find(id);
		return found == paths.end() ? std::nullopt : std::optional{ found->second };
	}
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

// The resolver's write policy is the second wall behind the argument check an agent's file.save_as goes through
// (edt::check_agent_level_path): a path the check lets through but the policy does not allow is not written either.
BOOST_AUTO_TEST_CASE(SaveAs_TheWritePolicyRefusesWhatTheArgumentCheckAllows)
{
	rs->unregistrate_resolver(res::tag::default_protocol());
	rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ content },
		res::mount_access::read_write, res::write_policy(res::tag::default_protocol(), { "res://levels/**", "!res://levels/locked/**" }));

	for (const char* path : { "other/x.desc", "levels/locked/x.desc", "x.desc" }) {
		BOOST_TEST_INFO("path: " << path);
		BOOST_TEST(edt::check_agent_level_path(path).ok());
		BOOST_TEST(!level_ctrl.save_level_as(path));
		BOOST_TEST(!fs::exists(content / path));
	}
	BOOST_TEST(!state.level_tag.is_valid());

	BOOST_REQUIRE(level_ctrl.save_level_as("levels/ok.desc"));
	BOOST_TEST(fs::exists(content / "levels" / "ok.desc"));
}

BOOST_AUTO_TEST_CASE(SaveAs_TheResolverRefusesWhatTheArgumentCheckRefuses)
{
	const fs::path absolute = dir.path() / "absolute.desc";
	for (const std::string& path : { std::string{ "../escape.desc" }, std::string{ "levels/../../escape.desc" },
			absolute.generic_string(), std::string{ "memory://levels/x.desc" } }) {
		BOOST_TEST_INFO("path: " << path);
		BOOST_TEST(!edt::check_agent_level_path(path).ok());
		BOOST_TEST(!level_ctrl.save_level_as(path));
	}
	BOOST_TEST(!fs::exists(dir.path() / "escape.desc"));
	BOOST_TEST(!fs::exists(absolute));
}

BOOST_AUTO_TEST_CASE(SaveAs_RefusesALevelBehindALinkedFolder)
{
	dir.write("outside/keep.txt", "keep");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", content / "linked", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	BOOST_TEST(edt::check_agent_level_path("linked/x.desc").ok());
	BOOST_TEST((edt::inspect_level_file(content, "linked/x.desc") == edt::level_file_state::unsafe));
	BOOST_TEST(!level_ctrl.save_level_as("linked/x.desc"));
	BOOST_TEST(!fs::exists(dir.path() / "outside" / "x.desc"));
}

// #812: a level read on top of a parent is saved, by Save and Save As, as its __parent and what differs from the parent.
BOOST_AUTO_TEST_CASE(ALevelWithAParent_IsSavedWithItsParentAndOnlyItsOwnFields)
{
	dir.write("content/levels/base.desc", R"({"__type":"level_desc","name":"base","worlds":[]})");
	dir.write("content/levels/child.desc", R"({"__type":"level_desc","__parent":"res://levels/base.desc","name":"child"})");
	const auto child = res::tag::make("levels/child.desc");
	state.editor_tag = child;
	state.level_tag = child;
	auto handle = rs->require_sync<scn::level_desc>(child);
	pump();
	BOOST_REQUIRE(handle.is_ready());
	BOOST_REQUIRE(level_ctrl.finish_level_load(handle));
	BOOST_TEST((state.level_parent == res::tag::make("levels/base.desc")));

	state.commit("edit", edt::commit_mode::state_only);
	BOOST_REQUIRE(level_ctrl.save_level_as("levels/saved.desc"));
	const auto saved = boost::json::parse(read_file(content / "levels" / "saved.desc")).as_object();
	BOOST_TEST(saved.at("__parent").as_string() == "res://levels/base.desc");
	BOOST_TEST(saved.at("name").as_string() == "saved");
	BOOST_TEST(!saved.contains("worlds"));

	BOOST_REQUIRE(level_ctrl.quick_save_level());
	const auto again = boost::json::parse(read_file(content / "levels" / "saved.desc")).as_object();
	BOOST_TEST(again.at("__parent").as_string() == "res://levels/base.desc");
	BOOST_TEST(!again.contains("worlds"));
}

BOOST_AUTO_TEST_SUITE_END()

// #772: the ID of a level is its .resource. A save keeps the __guid its file had until the project's migration takes it
// out, and never makes one up; Save As over another file keeps that file's.
BOOST_FIXTURE_TEST_SUITE(EdtLevelGuidTests, level_guid_fixture)

BOOST_AUTO_TEST_CASE(SaveAs_ToANewFile_WritesNoGuid)
{
	state.level_tag = res::tag::make("levels/mine.desc");
	state.level_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
	BOOST_REQUIRE(level_ctrl.save_level_as("levels/copy.desc"));
	BOOST_TEST(!saved("levels/copy.desc").contains("__guid"));
	BOOST_TEST(state.level_guid.empty());
}

BOOST_AUTO_TEST_CASE(Save_KeepsTheGuidTheFileHad_AndMakesNoneUp)
{
	state.level_tag = res::tag::make("levels/mine.desc");
	state.level_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
	BOOST_REQUIRE(level_ctrl.quick_save_level());
	BOOST_TEST(saved("levels/mine.desc").at("__guid").as_string() == "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");

	state.level_guid.clear();
	BOOST_REQUIRE(level_ctrl.quick_save_level());
	BOOST_TEST(!saved("levels/mine.desc").contains("__guid"));
}

BOOST_AUTO_TEST_CASE(SaveAs_OverAnotherFile_KeepsThatFilesGuid)
{
	write("levels/other.desc", R"({"__type":"level_desc","__guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","name":"other","worlds":[]})");
	write("levels/plain.desc", R"({"__type":"level_desc","name":"plain","worlds":[]})");
	state.level_tag = res::tag::make("levels/mine.desc");
	state.level_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";

	BOOST_REQUIRE(level_ctrl.save_level_as("levels/other.desc"));
	BOOST_TEST(saved("levels/other.desc").at("__guid").as_string() == "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6");
	BOOST_TEST(state.level_guid == "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6");

	BOOST_REQUIRE(level_ctrl.save_level_as("levels/plain.desc"));
	BOOST_TEST(!saved("levels/plain.desc").contains("__guid"));
}

// A file of a lower layer is replaced by a copy in the project's own folder, which pairs with the same .resource.
BOOST_AUTO_TEST_CASE(SaveAs_OverAFileOfALowerLayer_KeepsItsGuid)
{
	write("levels/base.desc", R"({"__type":"level_desc","__guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","name":"base","worlds":[]})", lower);
	BOOST_REQUIRE(level_ctrl.save_level_as("levels/base.desc"));
	BOOST_TEST(saved("levels/base.desc").at("__guid").as_string() == "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6");
}

// The size limit of desc.* calls does not turn the rule off: the __guid of a big file is kept too.
BOOST_AUTO_TEST_CASE(SaveAs_OverABigFile_KeepsItsGuid)
{
	const std::string padding(2 * 1024 * 1024, 'x');
	write("levels/big.desc", R"({"__type":"level_desc","__guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","name":"big","pad":")" + padding + R"(","worlds":[]})");
	BOOST_REQUIRE(level_ctrl.save_level_as("levels/big.desc"));
	BOOST_TEST(saved("levels/big.desc").at("__guid").as_string() == "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6");
}

// A file too big to be a level is not read for its __guid: Save As over it is refused and the file stays as it was.
BOOST_AUTO_TEST_CASE(SaveAs_OverAFileBiggerThan64MiB_IsRefused)
{
	{
		fs::create_directories(content / "levels");
		std::ofstream out(content / "levels" / "huge.desc", std::ios::binary);
		out << R"({"__type":"level_desc","__guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","pad":")";
		const std::string chunk(1 << 20, 'x');
		for (int i = 0; i < 65; ++i) out << chunk;
		out << R"("})";
	}
	const auto size = fs::file_size(content / "levels" / "huge.desc");
	state.commit("edit", edt::commit_mode::state_only);
	BOOST_TEST(!level_ctrl.save_level_as("levels/huge.desc"));
	BOOST_TEST(state.is_dirty());
	BOOST_TEST(fs::file_size(content / "levels" / "huge.desc") == size);
	fs::remove(content / "levels" / "huge.desc");
}

// #773: a level save writes a reference whose target has an ID as {guid, path}, and the next save gives the same bytes.
BOOST_AUTO_TEST_CASE(Save_WritesTheIdOfAReferencedFile)
{
	scn::prefab_desc::prefab_comp_node sky;
	sky.type_name = "skybox_desc";
	sky.overrides["material"] = "res://sky.mat.desc";
	// A guid the index does not know: written back as it is.
	const boost::json::value unknown = boost::json::parse(R"({"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","path":"res://gone.mat.desc"})");
	scn::prefab_desc::prefab_comp_node lost_sky;
	lost_sky.type_name = "skybox_desc";
	lost_sky.overrides["material"] = unknown;
	scn::world_desc world;
	world.get_root().node_id = 1;
	world.get_root().components["sky"] = std::move(sky);
	world.get_root().components["sky_lost"] = std::move(lost_sky);
	// No world name: no world is loaded here, so the save has no live transforms to take over.
	state.world_descs.push_back(std::move(world));
	state.level_tag = res::tag::make("levels/sky.desc");
	state.level_guid = "11111111-2222-4333-8444-555555555555";

	const res::asset_id id = *res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	auto index = std::make_shared<ids_index>();
	index->hold(id, "res://sky.mat.desc");
	rs->set_asset_index(index);
	const bool first = level_ctrl.quick_save_level();
	const std::string written = read_file(content / "levels" / "sky.desc");
	const bool second = level_ctrl.quick_save_level();
	rs->set_asset_index(nullptr);

	BOOST_REQUIRE(first);
	const boost::json::value file = boost::json::parse(written);
	boost::system::error_code ec_of_pointer;
	const boost::json::value* sky_json = file.find_pointer("/worlds/0/components/sky", ec_of_pointer);
	const boost::json::value* lost_json = file.find_pointer("/worlds/0/components/sky_lost", ec_of_pointer);
	BOOST_REQUIRE(sky_json);
	BOOST_REQUIRE(lost_json);
	BOOST_TEST(sky_json->at("material") == boost::json::value(boost::json::object{ { "guid", res::to_text(id) }, { "path", "res://sky.mat.desc" } }));
	BOOST_TEST(lost_json->at("material") == unknown);
	BOOST_TEST(file.at("__guid") == boost::json::value("11111111-2222-4333-8444-555555555555"), "the level's own __guid stays with an index");
	BOOST_REQUIRE(second);
	BOOST_TEST(read_file(content / "levels" / "sky.desc") == written);
}

BOOST_AUTO_TEST_SUITE_END()
