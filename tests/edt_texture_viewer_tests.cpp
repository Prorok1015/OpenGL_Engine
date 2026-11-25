#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "edt_document_creator.h"
#include "edt_scene_document.h"
#include "edt_texture_viewer_host.h"
#include "edt_texture_viewer_panel.h"
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The read-only texture viewer (#1205): a picture with Create Texture (through document_creator, beside the picture, never
// over a file, not for a picture of a lower layer nor while the project may not be written) and a texture desc of a lower
// layer, which is no data document.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

struct viewer_fixture : test_util::null_engine_fixture
{
	viewer_fixture()
		: null_engine_fixture(false)
		, gui(nullptr, nullptr)
		, services{ .ctx_manager = storage.require_shared<scn::context_manager>(),
			.base_descs = &storage.require<desc::desc_system>(), .base_resources = &storage.require<res::resource_system>(),
			.base_assembler = &storage.require<scn::ecs_assembler>(), .base_sfactory = &storage.require<ecs::system_factory>() }
		, context(std::make_unique<edt::editor_context>(
			storage.require<res::resource_system>(), storage.require<rnd::render_system>(),
			gui, services, tasks, commands, scene))
	{
		fs::create_directories(res_root);
		fs::create_directories(lower);
		auto& resources = storage.require<res::resource_system>();
		resources.unregistrate_resolver(res::tag::default_protocol());
		resources.registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		auto& descs = storage.require<desc::desc_system>();
		creator = std::make_unique<edt::document_creator>(edt::document_creator::ports{
			[&descs] { return &descs; }, [&resources] { return &resources; },
			[this] { return std::vector<fs::path>{ lower, res_root }; },
			[this]() -> std::optional<std::string> { return blocker; } });
		host = std::make_unique<edt::texture_viewer_host>(edt::texture_viewer_host::ports{
			.context = *context,
			.services = services,
			.layout = [this] { return layout(); },
			.creator = [this]() -> const edt::document_creator* { return creator.get(); },
			.open_texture = [this](const res::tag& tag) { opened.emplace_back(tag.view()); } });
	}

	~viewer_fixture()
	{
		host.reset();
		context->documents().shutdown();
		storage.require<res::resource_system>().unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout() const
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	static res::tag write(const fs::path& root, std::string_view name, std::string_view content)
	{
		fs::create_directories((root / std::string(name)).parent_path());
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
		return res::tag{ "res://" + std::string(name) };
	}

	std::string read(std::string_view name) const
	{
		std::ifstream in(res_root / std::string(name), std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	test_util::temp_dir dir{ "edt_texture_viewer" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	gui::gui_system gui;
	cmd::command_registry commands;
	edt::editor_services services;
	edt::scene_document scene;
	std::unique_ptr<edt::editor_context> context;
	std::optional<std::string> blocker;
	std::unique_ptr<edt::document_creator> creator;
	std::unique_ptr<edt::texture_viewer_host> host;
	std::vector<std::string> opened;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtTextureViewerTests, viewer_fixture)

BOOST_AUTO_TEST_CASE(ThePlannedTexture_IsBesideThePicture_ANameOfItsStem_NamingThePicture)
{
	const edt::picture_texture planned = edt::picture_texture_of(res::tag{ "res://pictures/stone/rock.png" });
	BOOST_TEST(planned.folder == "pictures/stone");
	BOOST_TEST(planned.name == "rock.txm.desc");
	BOOST_TEST(planned.fields.at("__parent").as_string() == "res://base_texture.desc");
	BOOST_TEST(planned.fields.at("data").as_string() == "res://pictures/stone/rock.png");
	BOOST_TEST(edt::picture_texture_of(res::tag{ "res://top.pct" }).folder.empty());
}

BOOST_AUTO_TEST_CASE(CreateTexture_WritesTheDescBesideThePicture_ThenOpensIt_AndIsNotOfferedTwice)
{
	const res::tag picture = write(res_root, "pictures/rock.png", "not decoded here");
	host->open_picture(picture);
	edt::texture_viewer_panel* viewer = host->find(picture);
	BOOST_REQUIRE(viewer != nullptr);
	BOOST_TEST(context->panels().find(viewer->get_id()) == viewer);
	BOOST_TEST(viewer->refusal().empty(), viewer->refusal());
	BOOST_TEST(!viewer->has_existing());

	BOOST_REQUIRE(host->create_for(picture));
	BOOST_TEST((opened == std::vector<std::string>{ "res://pictures/rock.txm.desc" }));
	const json::object written = json::parse(read("pictures/rock.txm.desc")).as_object();
	BOOST_TEST(written.at("__type").as_string() == "texture_2d_desc");
	BOOST_TEST(written.at("__version").to_number<int>() == storage.require<desc::desc_system>().current_version("texture_2d_desc"));
	BOOST_TEST(written.at("__parent").as_string() == "res://base_texture.desc");
	BOOST_TEST(written.at("data").as_string() == "res://pictures/rock.png");
	BOOST_TEST(written.size() == 4u);

	// The texture is there now: the viewer offers to open it, and a second Create writes nothing.
	BOOST_TEST(viewer->has_existing());
	BOOST_TEST((host->texture_of_picture(picture) == res::tag{ "res://pictures/rock.txm.desc" }));
	const std::string before = read("pictures/rock.txm.desc");
	BOOST_TEST(!host->create_for(picture));
	BOOST_TEST(read("pictures/rock.txm.desc") == before);
	BOOST_TEST(opened.size() == 1u);
}

BOOST_AUTO_TEST_CASE(CreateTexture_IsRefused_ForAPictureOfALowerLayer_AndWhileTheProjectMayNotBeWritten)
{
	const res::tag engine_picture = write(lower, "engine.png", "not decoded here");
	host->open_picture(engine_picture);
	edt::texture_viewer_panel* viewer = host->find(engine_picture);
	BOOST_REQUIRE(viewer != nullptr);
	BOOST_TEST(!viewer->refusal().empty());

	const res::tag picture = write(res_root, "own.png", "not decoded here");
	blocker = "a project migration is running";
	host->open_picture(picture);
	edt::texture_viewer_panel* blocked = host->find(picture);
	BOOST_REQUIRE(blocked != nullptr);
	BOOST_TEST(blocked->refusal() == "a project migration is running");
	BOOST_TEST(!host->create_for(picture));
	BOOST_TEST(!fs::exists(res_root / "own.txm.desc"));
	BOOST_TEST(opened.empty());
}

// A texture beside the picture that names it is opened from the viewer; one that names another picture refuses the name.
BOOST_AUTO_TEST_CASE(AnExistingTexture_OfThePictureIsOpened_OfAnotherIsAReason)
{
	const res::tag rock = write(res_root, "rock.png", "not decoded here");
	write(res_root, "rock.txm.desc",
		R"json({"__type":"texture_2d_desc","__version":1,"__parent":"res://base_texture.desc","data":"res://rock.png"})json");
	host->open_picture(rock);
	edt::texture_viewer_panel* own = host->find(rock);
	BOOST_REQUIRE(own != nullptr);
	BOOST_TEST(own->has_existing());

	const res::tag sand = write(res_root, "sand.png", "not decoded here");
	write(res_root, "sand.txm.desc", R"json({"__type":"texture_2d_desc","__version":1,"data":"res://other.png"})json");
	host->open_picture(sand);
	edt::texture_viewer_panel* other = host->find(sand);
	BOOST_REQUIRE(other != nullptr);
	BOOST_TEST(!other->has_existing());
	BOOST_TEST(other->refusal().find("already exists") != std::string::npos, other->refusal());
	BOOST_TEST(!host->create_for(sand));
	BOOST_TEST(opened.empty());
}

BOOST_AUTO_TEST_CASE(ATextureOfALowerLayer_IsShownReadOnly_AndClosingTheViewerLetsItGo)
{
	const res::tag engine_texture = write(lower, "engine.txm.desc",
		R"json({"__type":"texture_2d_desc","__version":1,"header":{"wrap":"repeat","data":{"mip_levels":3}}})json");
	const res::tag own_texture = write(res_root, "own.txm.desc", R"json({"__type":"texture_2d_desc","__version":1})json");
	BOOST_TEST(host->is_lower_layer_desc(engine_texture));
	BOOST_TEST(!host->is_lower_layer_desc(own_texture));
	BOOST_TEST(!host->is_lower_layer_desc(res::tag{ "res://nothing.txm.desc" }));

	host->open_desc(engine_texture);
	edt::texture_viewer_panel* viewer = host->find(engine_texture);
	BOOST_REQUIRE(viewer != nullptr);
	const std::string id = viewer->get_id();
	// Opened again: the same window.
	host->open_desc(engine_texture);
	BOOST_TEST(host->find(engine_texture) == viewer);

	host->release_previews();
	host->rebuild_previews();
	host->close(engine_texture);
	BOOST_TEST(!host->is_open(engine_texture));
	BOOST_TEST(context->panels().find(id) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
