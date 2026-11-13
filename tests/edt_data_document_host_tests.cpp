#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_data_document_host.h"
#include "edt_scene_document.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/ecs/ecs_system.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// data_document_host (#818): opening what is open moves the focus, a file that is no data document makes none, and
// closing a document with unsaved changes asks first.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr const char* k_config_schema = R"({
	"name": "test.config", "kind": "desc", "is_document": true,
	"fields": [ { "name": "speed", "kind": "f32", "default": 1.0 } ]
})";

constexpr const char* k_plain_schema = R"({
	"name": "test.plain", "kind": "desc",
	"fields": [ { "name": "speed", "kind": "f32", "default": 1.0 } ]
})";

struct host_fixture : test_util::null_engine_fixture
{
	host_fixture()
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
		auto& resources = storage.require<res::resource_system>();
		resources.unregistrate_resolver(res::tag::default_protocol());
		resources.registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		auto& descs = storage.require<desc::desc_system>();
		descs.register_schema(desc::parse_type_schema(json::parse(k_config_schema).as_object()));
		descs.register_schema(desc::parse_type_schema(json::parse(k_plain_schema).as_object()));
		context->dialogs().set_registrar([](std::string_view, std::function<bool()>) {});

		edt::desc_file_layout layout;
		layout.top_layer = [this] { return res_root; };
		layout.layers = [this] { return std::vector<fs::path>{ res_root }; };
		layout.open_level_files = [] { return std::vector<std::string>{}; };
		host = std::make_unique<edt::data_document_host>(*context, services, [layout] { return layout; },
			[this](edt::document& open) { ++focused_elsewhere; context->documents().set_active(open.id()); });
	}

	~host_fixture()
	{
		host.reset();
		context->documents().shutdown();
		storage.require<res::resource_system>().unregistrate_resolver(res::tag::default_protocol());
	}

	res::tag put(std::string_view name, std::string_view content) const
	{
		std::ofstream out(res_root / std::string(name), std::ios::binary);
		out << content;
		return res::tag{ "res://" + std::string(name) };
	}

	edt::document& open_config(std::string_view name = "config.desc")
	{
		const res::tag tag = put(name, R"({"__type":"test.config","speed":2.0})");
		host->open(tag, false);
		edt::document* found = context->documents().find_by_source(tag);
		BOOST_REQUIRE(found != nullptr);
		return *found;
	}

	void make_dirty(edt::document& document)
	{
		auto& data = static_cast<edt::data_document&>(document);
		BOOST_REQUIRE(data.edit([](json::value& shown) {
			shown.as_object()["speed"] = 5.0;
			return true;
		}));
		BOOST_REQUIRE(document.is_dirty());
	}

	void answer_unsaved(std::string_view choice)
	{
		const auto pending = context->dialogs().list();
		BOOST_REQUIRE(pending.size() == 1u);
		BOOST_REQUIRE(pending.front().kind == "unsaved");
		BOOST_REQUIRE(context->dialogs().answer(pending.front().id, json::object{ { "choice", std::string(choice) } }).ok);
	}

	std::size_t open_documents()
	{
		std::size_t count = 0;
		context->documents().for_each([&count](edt::document&) { ++count; });
		return count;
	}

	test_util::temp_dir dir{ "edt_data_document_host" };
	const fs::path res_root = dir.path() / "res";
	gui::gui_system gui;
	cmd::command_registry commands;
	edt::editor_services services;
	edt::scene_document scene;
	std::unique_ptr<edt::editor_context> context;
	std::unique_ptr<edt::data_document_host> host;
	int focused_elsewhere = 0;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDataDocumentHostTests, host_fixture)

BOOST_AUTO_TEST_CASE(Open_OfAFileThatIsOpen_FocusesItAndMakesNoSecondDocument)
{
	edt::document& first = open_config();
	const edt::document_id id = first.id();
	BOOST_REQUIRE(host->owns(id));
	BOOST_TEST(open_documents() == 2u);

	context->documents().set_active(context->documents().scene().id());
	host->open(res::tag{ "res://./config.desc" }, false);

	BOOST_TEST(open_documents() == 2u);
	BOOST_TEST(context->documents().active() == &first);
}

BOOST_AUTO_TEST_CASE(Open_OfAFileThatIsNoDataDocument_OpensNothingAndOnlyLogsIt)
{
	const res::tag plain = put("plain.desc", R"({"__type":"test.plain","speed":2.0})");

	host->open(plain, true);
	host->open(plain, false);
	BOOST_TEST(open_documents() == 1u);
	BOOST_TEST(context->dialogs().pending() == 0u);
}

BOOST_AUTO_TEST_CASE(Open_OfADocumentThatIsBroken_ShowsAMessageUnlessQuiet)
{
	const res::tag broken = put("broken.desc", R"({"__type":"test.config","__parent":"res://missing.desc"})");

	host->open(broken, true);
	BOOST_TEST(context->dialogs().pending() == 0u);

	host->open(broken, false);
	BOOST_TEST(open_documents() == 1u);
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_TEST(context->dialogs().list().front().kind == "message");
}

BOOST_AUTO_TEST_CASE(Close_OfACleanDocument_ClosesAtOnce)
{
	edt::document& open = open_config();
	host->request_close(open.id());
	BOOST_TEST(open_documents() == 1u);
	BOOST_TEST(context->dialogs().pending() == 0u);
}

BOOST_AUTO_TEST_CASE(Close_OfADirtyDocument_AsksFirst_CancelKeepsIt_DiscardClosesIt)
{
	edt::document& open = open_config();
	const edt::document_id id = open.id();
	make_dirty(open);

	host->request_close(id);
	BOOST_TEST(open_documents() == 2u);
	answer_unsaved("cancel");
	BOOST_TEST(open_documents() == 2u);
	BOOST_TEST(host->owns(id));

	host->request_close(id);
	answer_unsaved("discard");
	BOOST_TEST(open_documents() == 1u);
	BOOST_TEST(!host->owns(id));
}

BOOST_AUTO_TEST_CASE(Close_OfADirtyDocument_Save_WritesItThenCloses_AFailedSaveKeepsItOpen)
{
	edt::document& open = open_config();
	const edt::document_id id = open.id();
	make_dirty(open);

	{
		std::ofstream out(res_root / "config.desc", std::ios::binary | std::ios::trunc);
		out << R"({"__type":"test.config","speed":9.0,"extra":1})";
	}
	host->request_close(id);
	answer_unsaved("save");
	BOOST_TEST(open_documents() == 2u);
	BOOST_TEST(host->owns(id));
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_TEST(context->dialogs().list().front().kind == "message");
}

BOOST_AUTO_TEST_CASE(DirtyAndSaveAll_CoverEveryOpenDataDocument)
{
	edt::document& first = open_config("a.desc");
	edt::document& second = open_config("b.desc");
	BOOST_TEST(host->dirty().empty());
	make_dirty(second);
	BOOST_TEST((host->dirty() == std::vector<edt::document_id>{ second.id() }));

	BOOST_TEST(host->save_all());
	BOOST_TEST(host->dirty().empty());
	BOOST_TEST(!first.is_dirty());
}

BOOST_AUTO_TEST_SUITE_END()
