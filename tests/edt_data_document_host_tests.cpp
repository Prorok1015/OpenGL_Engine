#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_data_document_host.h"
#include "edt_data_document_panel.h"
#include "edt_data_document_view.h"
#include "edt_scene_document.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/ecs/ecs_system.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <boost/json.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <iterator>
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

	std::string read_text(std::string_view name) const
	{
		std::ifstream in(res_root / std::string(name), std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
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

BOOST_AUTO_TEST_CASE(Close_OfADirtyDocument_Save_WritesItThenCloses_AChangedFileIsAskedAbout)
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
	// the file changed on disk: the refused save is a question now, and Cancel keeps the window and the edits
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_TEST(context->dialogs().list().front().kind == "choice");
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 2 } }).ok);
	BOOST_TEST(open_documents() == 2u);
	BOOST_TEST(host->owns(id));
	BOOST_TEST(open.is_dirty());
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

BOOST_AUTO_TEST_CASE(Save_OfAFileThatChangedOnDisk_AsksAndOverwriteWritesTheDocument)
{
	edt::document& open = open_config();
	make_dirty(open);
	put("config.desc", R"({"__type":"test.config","speed":4.0})");

	host->request_save(open.id());
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().list().front().kind == "choice");
	BOOST_TEST(open.is_dirty());

	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 1 } }).ok);
	BOOST_TEST(!open.is_dirty());
	BOOST_TEST(read_text("config.desc").find("5.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Save_OfAFileThatChangedOnDisk_ReloadTakesTheFile_CancelChangesNothing)
{
	edt::document& open = open_config();
	make_dirty(open);
	put("config.desc", R"({"__type":"test.config","speed":4.0})");

	host->request_save(open.id());
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 2 } }).ok);
	BOOST_TEST(open.is_dirty());

	host->request_save(open.id());
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 0 } }).ok);
	BOOST_TEST(!open.is_dirty());
	BOOST_TEST(read_text("config.desc").find("4.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheBannersReload_OfACleanDocument_IsSilent_OfADirtyOne_AsksFirst)
{
	edt::document& open = open_config();
	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	host->request_reload(open.id());
	BOOST_TEST(context->dialogs().pending() == 0u);

	make_dirty(open);
	host->request_reload(open.id());
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 1 } }).ok);
	BOOST_TEST(open.is_dirty());

	host->request_reload(open.id());
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 0 } }).ok);
	BOOST_TEST(!open.is_dirty());
}

BOOST_AUTO_TEST_CASE(TheBannersOverwrite_AlwaysAsks_AndCancelWritesNothing)
{
	edt::document& open = open_config();
	make_dirty(open);
	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	const std::string on_disk = read_text("config.desc");

	host->request_overwrite(open.id());
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 1 } }).ok);
	BOOST_TEST(read_text("config.desc") == on_disk);
	BOOST_TEST(open.is_dirty());
}

BOOST_AUTO_TEST_CASE(ClosePrompt_Save_OfAFileThatChangedOnDisk_AsksAgain_OverwriteClosesCancelKeepsTheWindow)
{
	edt::document& open = open_config();
	const edt::document_id id = open.id();
	make_dirty(open);
	put("config.desc", R"({"__type":"test.config","speed":4.0})");

	host->request_close(id);
	answer_unsaved("save");
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().list().front().kind == "choice");
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 2 } }).ok);
	BOOST_TEST(open_documents() == 2u);
	BOOST_TEST(host->owns(id));

	host->request_close(id);
	answer_unsaved("save");
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 1 } }).ok);
	BOOST_TEST(open_documents() == 1u);
	BOOST_TEST(read_text("config.desc").find("5.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(SaveAll_OfAFileThatChangedOnDisk_AsksAndDoesNotLetTheExitGoOn)
{
	edt::document& open = open_config();
	make_dirty(open);
	put("config.desc", R"({"__type":"test.config","speed":4.0})");

	BOOST_TEST(!host->save_all());
	BOOST_REQUIRE(context->dialogs().list().size() == 1u);
	BOOST_REQUIRE(context->dialogs().list().front().kind == "choice");
	BOOST_REQUIRE(context->dialogs().answer(context->dialogs().list().front().id, json::object{ { "index", 2 } }).ok);
	BOOST_TEST(open.is_dirty());
	BOOST_TEST(read_text("config.desc").find("4.0") != std::string::npos);
}

// The active document follows the focus of the data windows (S56-5): Edit > Undo acts on the window the person works in.
namespace {

struct imgui_context
{
	imgui_context() : handle(ImGui::CreateContext())
	{
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* pixels = nullptr;
		int width = 0;
		int height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}
	~imgui_context() { ImGui::DestroyContext(handle); }
	ImGuiContext* handle;
};

}

BOOST_AUTO_TEST_CASE(TheActiveDocumentFollowsTheFocusOfTwoDataWindows_InEitherRenderOrder)
{
	imgui_context imgui;
	edt::document& first = open_config("a.desc");
	edt::document& second = open_config("b.desc");
	edt::panel_base* a = context->panels().find(edt::data_document_panel_id(first.id()));
	edt::panel_base* b = context->panels().find(edt::data_document_panel_id(second.id()));
	BOOST_REQUIRE(a != nullptr);
	BOOST_REQUIRE(b != nullptr);
	edt::document_manager& documents = context->documents();

	const auto frame = [this](edt::panel_base* one, edt::panel_base* two) {
		ImGui::NewFrame();
		one->render();
		two->render();
		host->follow_focus();
		ImGui::EndFrame();
	};
	const auto focus = [](edt::panel_base* panel) { ImGui::SetWindowFocus(panel->get_title().c_str()); };
	frame(a, b);
	frame(a, b);

	focus(a);
	frame(a, b);
	BOOST_TEST(documents.active() == &first);

	// The window that loses the focus is drawn first, the one that gets it second: the second has the last word.
	focus(b);
	frame(a, b);
	BOOST_TEST(documents.active() == &second);

	// The window that gets the focus is drawn first: the one that loses it must not take the activity away again.
	focus(a);
	frame(a, b);
	BOOST_TEST(documents.active() == &first);
	focus(b);
	frame(b, a);
	BOOST_TEST(documents.active() == &second);
	focus(a);
	frame(a, b);
	BOOST_TEST(documents.active() == &first);
}

BOOST_AUTO_TEST_CASE(AnotherWindowTakingTheFocus_GivesTheSceneTheActivityBack)
{
	imgui_context imgui;
	edt::document& data = open_config();
	edt::panel_base* panel = context->panels().find(edt::data_document_panel_id(data.id()));
	BOOST_REQUIRE(panel != nullptr);
	const auto frame = [&] {
		ImGui::NewFrame();
		panel->render();
		if (ImGui::Begin("viewport")) {
		}
		ImGui::End();
		host->follow_focus();
		ImGui::EndFrame();
	};
	frame();
	frame();
	ImGui::SetWindowFocus(panel->get_title().c_str());
	frame();
	BOOST_REQUIRE(context->documents().active() == &data);

	ImGui::SetWindowFocus("viewport");
	frame();
	BOOST_TEST(context->documents().active() == &context->documents().scene());
}

namespace {

// The main menu bar as the editor has it: a window of the dockspace host that takes no navigation focus, with Edit > Undo.
struct menu_bar_host
{
	explicit menu_bar_host(edt::document_manager& documents) : documents(documents) {}

	void draw()
	{
		ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(800.f, 600.f));
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
		ImGui::Begin("##dockspace_host", nullptr, flags);
		if (ImGui::BeginMenuBar()) {
			const bool open = ImGui::BeginMenu("Edit");
			edit_center = rect_center();
			if (open) {
				if (ImGui::MenuItem("Undo")) {
					++undo_clicks;
					if (edt::document* active = documents.active()) {
						if (active->undo()) ++undone;
					}
				}
				undo_center = rect_center();
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}
		ImGui::End();
	}

	static ImVec2 rect_center()
	{
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
	}

	edt::document_manager& documents;
	ImVec2 edit_center{ 0.f, 0.f };
	ImVec2 undo_center{ 0.f, 0.f };
	int undo_clicks = 0;
	int undone = 0;
};

}

BOOST_AUTO_TEST_CASE(EditUndoClickedWithTheMouse_TwiceInARow_UndoesTheDataDocumentBothTimes)
{
	imgui_context imgui;
	edt::document& open = open_config();
	auto& data = static_cast<edt::data_document&>(open);
	edt::panel_base* panel = context->panels().find(edt::data_document_panel_id(open.id()));
	BOOST_REQUIRE(panel != nullptr);
	edt::document_manager& documents = context->documents();
	menu_bar_host menu(documents);
	ImGuiIO& io = ImGui::GetIO();

	const auto frame = [&] {
		ImGui::NewFrame();
		menu.draw();
		panel->render();
		ImGui::SetNextWindowPos(ImVec2(620.f, 450.f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(150.f, 100.f), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Viewport")) {
		}
		ImGui::End();
		host->follow_focus();
		ImGui::EndFrame();
	};
	const auto frames = [&](int count) {
		for (int i = 0; i < count; ++i) frame();
	};
	const auto click = [&](ImVec2 at) {
		io.AddMousePosEvent(at.x, at.y);
		frames(2);
		io.AddMouseButtonEvent(0, true);
		frames(2);
		io.AddMouseButtonEvent(0, false);
		frames(2);
	};

	frames(3);
	ImGui::SetWindowFocus(panel->get_title().c_str());
	frames(3);
	BOOST_REQUIRE(documents.active() == &open);

	const double original = data.own().at("speed").to_number<double>();
	for (const double value : { 6.0, 7.0 }) {
		BOOST_REQUIRE(data.edit([value](json::value& shown) {
			shown.as_object()["speed"] = value;
			return true;
		}));
		data.commit_pending();
	}
	BOOST_REQUIRE(data.history().size() == 2u);

	for (int round = 1; round <= 2; ++round) {
		BOOST_TEST_CONTEXT("undo " << round) {
			click(menu.edit_center);
			BOOST_REQUIRE_MESSAGE(menu.undo_center.y > menu.edit_center.y, "the Edit menu did not open under the mouse click");
			click(menu.undo_center);
			frames(3);
			BOOST_TEST(menu.undo_clicks == round);
			BOOST_TEST(menu.undone == round);
			BOOST_TEST(documents.active() == &open);
		}
	}
	BOOST_TEST(data.own().at("speed").to_number<double>() == original);
	BOOST_TEST(documents.scene().next_undo_entry().has_value() == false);

	// The person goes on to the viewport: the next Undo is the scene's again, not the data window's.
	ImGui::SetWindowFocus("Viewport");
	frames(3);
	BOOST_TEST(documents.active() == &documents.scene());
}

namespace {

class fake_preview final : public edt::previewable_document
{
public:
	std::string_view kind() const override { return "prefab"; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return false; }
	std::uint64_t generation() const override { return 0; }
	void open(const res::tag& source) override { m_source = source; m_open = true; }
	void close() override { m_open = false; }
	bool is_open() const override { return m_open; }

private:
	std::string m_title = "preview";
	res::tag m_source;
	bool m_open = false;
};

class window_panel final : public edt::panel_base
{
public:
	window_panel(std::string_view id, std::string_view title) : panel_base(id, title) {}

protected:
	void on_render() override {}
};

}

BOOST_AUTO_TEST_CASE(ThePrefabPreviewWindowGivesItsDocument_TheViewportGivesTheSceneBack)
{
	imgui_context imgui;
	edt::document& data = open_config();
	edt::panel_base* panel = context->panels().find(edt::data_document_panel_id(data.id()));
	BOOST_REQUIRE(panel != nullptr);
	edt::document_manager& documents = context->documents();

	edt::asset_preview_desc kind;
	kind.id = "test.preview";
	kind.suffix = ".preview";
	kind.make_preview = [](edt::editor_services&) -> std::unique_ptr<edt::previewable_document> { return std::make_unique<fake_preview>(); };
	const edt::registration kind_registration = context->asset_kinds().add_preview(std::move(kind));
	edt::previewable_document* preview = documents.open_preview("test.preview", res::tag::make("a.preview"));
	BOOST_REQUIRE(preview != nullptr);
	auto prefab_window = std::make_shared<window_panel>("prefab_editor", "Prefab Editor");
	const edt::registration prefab_registration = context->panels().add({
		.id = "prefab_editor", .title = "Prefab Editor", .slot = edt::dock_slot::floating, .open_by_default = true,
		.show_in_view_menu = false, .panel = prefab_window });

	const auto frame = [&] {
		ImGui::NewFrame();
		panel->render();
		prefab_window->render();
		if (ImGui::Begin("Viewport")) {
		}
		ImGui::End();
		host->follow_focus();
		ImGui::EndFrame();
	};
	frame();
	frame();
	ImGui::SetWindowFocus(panel->get_title().c_str());
	frame();
	frame();
	BOOST_REQUIRE(documents.active() == &data);

	ImGui::SetWindowFocus("Prefab Editor");
	frame();
	frame();
	BOOST_TEST(documents.active() == preview);

	ImGui::SetWindowFocus("Viewport");
	frame();
	frame();
	BOOST_TEST(documents.active() == &documents.scene());

	ImGui::SetWindowFocus(panel->get_title().c_str());
	frame();
	frame();
	BOOST_TEST(documents.active() == &data);
}

BOOST_AUTO_TEST_CASE(AQuietlyOpenedWindow_TakesNeitherTheFocusNorTheActivity_InAnyNumberOfFrames)
{
	imgui_context imgui;
	const res::tag tag = put("quiet.desc", R"({"__type":"test.config","speed":2.0})");
	host->open(tag, true);
	edt::document* data = context->documents().find_by_source(tag);
	BOOST_REQUIRE(data != nullptr);
	edt::panel_base* panel = context->panels().find(edt::data_document_panel_id(data->id()));
	BOOST_REQUIRE(panel != nullptr);

	for (int i = 0; i < 6; ++i) {
		ImGui::NewFrame();
		if (ImGui::Begin("Viewport")) {
		}
		ImGui::End();
		panel->render();
		host->follow_focus();
		ImGui::EndFrame();
		BOOST_TEST(context->documents().active() == &context->documents().scene());
	}
}

BOOST_AUTO_TEST_CASE(AWindowTheFocusGoesToThatIsNoPlaceToWork_LeavesTheActiveDocumentAlone)
{
	imgui_context imgui;
	edt::document& data = open_config();
	edt::panel_base* panel = context->panels().find(edt::data_document_panel_id(data.id()));
	BOOST_REQUIRE(panel != nullptr);
	const auto frame = [&] {
		ImGui::NewFrame();
		panel->render();
		if (ImGui::Begin("##service")) {
		}
		ImGui::End();
		host->follow_focus();
		ImGui::EndFrame();
	};
	frame();
	frame();
	ImGui::SetWindowFocus(panel->get_title().c_str());
	frame();
	BOOST_REQUIRE(context->documents().active() == &data);

	ImGui::SetWindowFocus("##service");
	frame();
	frame();
	BOOST_TEST(context->documents().active() == &data);
}

BOOST_AUTO_TEST_CASE(AQuietOpen_LeavesTheActiveDocumentAlone_ALoudOneMakesTheNewDocumentActive)
{
	edt::document_manager& documents = context->documents();
	const edt::document_id scene_id = documents.scene().id();
	const res::tag quiet = put("quiet.desc", R"({"__type":"test.config","speed":2.0})");
	host->open(quiet, true);
	BOOST_REQUIRE(documents.find_by_source(quiet) != nullptr);
	BOOST_TEST(documents.active()->id() == scene_id);

	edt::document& loud = open_config("loud.desc");
	BOOST_TEST(documents.active() == &loud);
}

BOOST_AUTO_TEST_CASE(AnEditInTheMiddleOfADrag_IsRecordedWhenAnotherWindowTakesTheFocus)
{
	imgui_context imgui;
	edt::document& first = open_config("a.desc");
	edt::document& second = open_config("b.desc");
	auto& data = static_cast<edt::data_document&>(first);
	edt::panel_base* a = context->panels().find(edt::data_document_panel_id(first.id()));
	edt::panel_base* b = context->panels().find(edt::data_document_panel_id(second.id()));
	BOOST_REQUIRE(a != nullptr);
	BOOST_REQUIRE(b != nullptr);

	// A widget is held (a drag): the windows draw as usual, none of them takes the pending edit.
	const auto frame = [&](bool held) {
		ImGui::NewFrame();
		if (held) {
			ImGuiContext& g = *ImGui::GetCurrentContext();
			ImGui::SetActiveID(42, g.CurrentWindow);
		}
		a->render();
		b->render();
		host->follow_focus();
		ImGui::EndFrame();
	};
	frame(false);
	frame(false);
	ImGui::SetWindowFocus(a->get_title().c_str());
	frame(false);
	BOOST_REQUIRE(context->documents().active() == &first);

	BOOST_REQUIRE(data.edit([](json::value& shown) {
		shown.as_object()["speed"] = 7.0;
		return true;
	}));
	frame(true);
	BOOST_TEST(data.has_pending_edit());
	BOOST_TEST(data.history().size() == 0u);

	ImGui::SetWindowFocus(b->get_title().c_str());
	frame(true);
	BOOST_TEST(!data.has_pending_edit());
	BOOST_TEST(data.history().size() == 1u);
	BOOST_TEST(context->documents().active() == &second);
	BOOST_TEST(static_cast<edt::data_document&>(second).history().size() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
