#include <boost/test/unit_test.hpp>
#include "edt_document_tabs.h"
#include "edt_document_tabs_bar.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The strip of document tabs (#1184): which tabs there are, what a click and the close button do, and that the strip draws.

namespace {

class fake_document final : public edt::document
{
public:
	fake_document(std::string kind, std::string name, bool dirty = false)
		: m_kind(std::move(kind)), m_title(std::move(name)), m_source(res::tag::make(m_title + ".desc")), m_dirty(dirty) {}

	std::string_view kind() const override { return m_kind; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return m_dirty; }
	std::uint64_t generation() const override { return 0; }
	void bind_events(edt::editor_events*) override {}

	void set_dirty(bool dirty) { m_dirty = dirty; }

private:
	std::string m_kind;
	std::string m_title;
	res::tag m_source;
	bool m_dirty;
};

struct tabs_fixture
{
	tabs_fixture()
	{
		auto first = std::make_unique<fake_document>("data", "a");
		auto second = std::make_unique<fake_document>("data", "b", true);
		auto third = std::make_unique<fake_document>("prefab", "p");
		data_a = first.get();
		data_b = second.get();
		prefab = third.get();
		documents.adopt(std::move(first));
		documents.adopt(std::move(second));
		documents.adopt(std::move(third));
	}

	edt::document_tab_ports ports()
	{
		edt::document_tab_ports out;
		out.focus.show_data = [this](const res::tag& source) { shown_data.push_back(std::string(source.relative())); };
		out.focus.show_prefab = [this] { ++shown_prefab; };
		out.request_close_data = [this](edt::document_id id) { closed_data.push_back(id); };
		out.request_close_prefab = [this](edt::document_id) { ++closed_prefab; };
		out.focus.show_mesh = [this](edt::document_id id) { shown_mesh.push_back(id); };
		out.request_close_mesh = [this](edt::document_id id) { closed_mesh.push_back(id); };
		return out;
	}

	fake_document scene{ "scene", "level" };
	edt::editor_events events;
	edt::document_manager documents{ scene, events };
	fake_document* data_a = nullptr;
	fake_document* data_b = nullptr;
	fake_document* prefab = nullptr;
	std::vector<std::string> shown_data;
	int shown_prefab = 0;
	std::vector<edt::document_id> closed_data;
	int closed_prefab = 0;
	std::vector<edt::document_id> shown_mesh;
	std::vector<edt::document_id> closed_mesh;
};

struct imgui_frame
{
	imgui_frame()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}
	~imgui_frame() { ImGui::DestroyContext(ctx); }

	template <class draw>
	void frame(draw&& body)
	{
		ImGui::NewFrame();
		ImGui::Begin("host");
		body();
		ImGui::End();
		ImGui::EndFrame();
	}

	ImGuiContext* ctx = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDocumentTabs, tabs_fixture)

BOOST_AUTO_TEST_CASE(Tabs_AreTheOpenDocuments_SceneFirst_ThenInTheOrderTheyWereOpened_WithTheirDirtyAndActiveMarks)
{
	const std::vector<edt::document_tab> tabs = edt::tabs_of(documents);

	BOOST_REQUIRE(tabs.size() == 4u);
	BOOST_TEST(tabs[0].id == scene.id());
	BOOST_TEST(tabs[0].kind == "scene");
	BOOST_TEST(tabs[0].title == "level");
	BOOST_TEST(tabs[0].active);
	BOOST_TEST(!tabs[0].closable);
	BOOST_TEST(tabs[1].title == "a");
	BOOST_TEST(!tabs[1].dirty);
	BOOST_TEST(tabs[1].closable);
	BOOST_TEST(tabs[2].title == "b");
	BOOST_TEST(tabs[2].dirty);
	BOOST_TEST(tabs[3].kind == "prefab");
	BOOST_TEST(!tabs[1].active);
}

BOOST_AUTO_TEST_CASE(ATabThatBecomesDirty_IsMarked_AndTheActiveMarkFollowsTheActiveDocument)
{
	data_a->set_dirty(true);
	documents.set_active(data_b->id());

	const std::vector<edt::document_tab> tabs = edt::tabs_of(documents);

	BOOST_TEST(tabs[1].dirty);
	BOOST_TEST(!tabs[0].active);
	BOOST_TEST(tabs[2].active);
}

BOOST_AUTO_TEST_CASE(ClickingATab_FocusesThatDocument_ADataWindowThroughItsHost_ThePrefabAndTheSceneBySettingItActive)
{
	edt::document_tabs tabs(documents, ports());

	tabs.activate(data_a->id());
	BOOST_TEST((shown_data == std::vector<std::string>{ "a.desc" }));
	BOOST_TEST(documents.active() == &scene);

	tabs.activate(prefab->id());
	BOOST_TEST(shown_prefab == 1);
	BOOST_TEST(documents.active() == prefab);

	tabs.activate(scene.id());
	BOOST_TEST(documents.active() == &scene);
	BOOST_TEST(shown_prefab == 1);
}

BOOST_AUTO_TEST_CASE(ClickingATab_ChangesNoDocument_DirtyStateAndTitlesStayAsTheyWere)
{
	edt::document_tabs tabs(documents, ports());

	tabs.activate(data_b->id());
	tabs.activate(prefab->id());
	tabs.activate(scene.id());

	BOOST_TEST(data_b->is_dirty());
	BOOST_TEST(!data_a->is_dirty());
	BOOST_TEST(edt::tabs_of(documents).size() == 4u);
}

BOOST_AUTO_TEST_CASE(ATabThatNamesNoOpenDocument_IsIgnored)
{
	edt::document_tabs tabs(documents, ports());

	tabs.activate(edt::document_id{ 9999 });

	BOOST_TEST(shown_data.empty());
	BOOST_TEST(shown_prefab == 0);
	BOOST_TEST(documents.active() == &scene);
	BOOST_TEST(!tabs.close(edt::document_id{ 9999 }));
}

BOOST_AUTO_TEST_CASE(TheCloseButton_AsksThroughTheDocumentsOwnCloseFlow_NothingIsClosedByTheTab)
{
	edt::document_tabs tabs(documents, ports());

	BOOST_TEST(tabs.close(data_b->id()));
	BOOST_TEST((closed_data == std::vector<edt::document_id>{ data_b->id() }));
	BOOST_TEST(tabs.close(prefab->id()));
	BOOST_TEST(closed_prefab == 1);

	BOOST_TEST(data_b->is_dirty());
	BOOST_TEST(edt::tabs_of(documents).size() == 4u);
}

BOOST_AUTO_TEST_CASE(ADocumentOfAnotherKind_HasNoCloseFlow_ItsTabIsNotClosableAndCloseDoesNothing)
{
	fake_document* other = new fake_document("model", "m");
	documents.adopt(std::unique_ptr<edt::document>(other));
	edt::document_tabs tabs(documents, ports());

	const std::vector<edt::document_tab> shown = tabs.tabs();

	BOOST_REQUIRE(shown.size() == 5u);
	BOOST_TEST(!shown.back().closable);
	BOOST_TEST(!tabs.close(other->id()));
	BOOST_TEST(closed_data.empty());
	BOOST_TEST(closed_prefab == 0);
}

BOOST_AUTO_TEST_CASE(AMeshTab_IsClosableThroughTheModellersQuestion_AndAClickShowsItsWindow)
{
	fake_document* mesh = new fake_document("mesh", "crate", true);
	documents.adopt(std::unique_ptr<edt::document>(mesh));
	edt::document_tabs tabs(documents, ports());

	const std::vector<edt::document_tab> shown = tabs.tabs();
	BOOST_REQUIRE(shown.size() == 5u);
	BOOST_TEST(shown.back().kind == "mesh");
	BOOST_TEST(shown.back().closable);
	BOOST_TEST(shown.back().dirty);

	tabs.activate(mesh->id());
	BOOST_REQUIRE_EQUAL(shown_mesh.size(), 1u);
	BOOST_TEST(shown_mesh.front() == mesh->id());

	BOOST_TEST(tabs.close(mesh->id()));
	BOOST_REQUIRE_EQUAL(closed_mesh.size(), 1u);
	BOOST_TEST(closed_mesh.front() == mesh->id());
	BOOST_TEST(documents.find(mesh->id()) == mesh);
}

BOOST_AUTO_TEST_CASE(WhileClosingIsBlocked_NoTabIsClosable_AndCloseAsksNothing)
{
	bool blocked = true;
	edt::document_tab_ports blocking = ports();
	blocking.close_blocked = [&blocked] { return blocked; };
	edt::document_tabs tabs(documents, blocking);

	for (const edt::document_tab& tab : tabs.tabs()) BOOST_TEST(!tab.closable);
	BOOST_TEST(!tabs.close(data_a->id()));
	BOOST_TEST(!tabs.close(prefab->id()));
	BOOST_TEST(closed_data.empty());
	BOOST_TEST(closed_prefab == 0);

	blocked = false;
	BOOST_TEST(tabs.close(data_a->id()));
	BOOST_TEST(closed_data.size() == 1u);
	BOOST_TEST(tabs.tabs()[1].closable);
}

BOOST_AUTO_TEST_CASE(TwoDocumentsWithOneTitle_AreTwoTabsWithTheirOwnIds)
{
	fake_document* twin = new fake_document("data", "a");
	documents.adopt(std::unique_ptr<edt::document>(twin));

	const std::vector<edt::document_tab> shown = edt::tabs_of(documents);

	BOOST_REQUIRE(shown.size() == 5u);
	BOOST_TEST(shown[1].title == shown[4].title);
	BOOST_TEST(shown[1].id != shown[4].id);
	edt::document_tabs tabs(documents, ports());
	tabs.activate(twin->id());
	BOOST_TEST((shown_data == std::vector<std::string>{ "a.desc" }));
	BOOST_TEST(tabs.close(twin->id()));
	BOOST_TEST((closed_data == std::vector<edt::document_id>{ twin->id() }));
}

BOOST_AUTO_TEST_CASE(TheSceneTab_CannotBeClosed)
{
	edt::document_tabs tabs(documents, ports());

	BOOST_TEST(!tabs.close(scene.id()));
	BOOST_TEST(closed_data.empty());
	BOOST_TEST(closed_prefab == 0);
}

BOOST_FIXTURE_TEST_CASE(TheStrip_DrawsWithoutCallingAnythingWhileNobodyClicks, imgui_frame)
{
	fake_document scene{ "scene", "level" };
	edt::editor_events events;
	edt::document_manager manager{ scene, events };
	manager.adopt(std::make_unique<fake_document>("data", "a", true));
	edt::document_tabs_bar bar;
	gui::theme_semantic_colors colors;
	int activations = 0;
	int closes = 0;
	const edt::document_tabs_bar::actions on{ [&](edt::document_id) { ++activations; }, [&](edt::document_id) { ++closes; } };

	for (int i = 0; i < 3; ++i) {
		frame([&] { bar.draw(edt::tabs_of(manager), colors, on); });
	}
	manager.set_active(manager.scene().id());
	frame([&] { bar.draw(edt::tabs_of(manager), colors, on); });

	BOOST_TEST(activations == 0);
	BOOST_TEST(closes == 0);
}

BOOST_FIXTURE_TEST_CASE(TheStrip_DrawsTwoTabsOfOneTitle_WithoutAClash, imgui_frame)
{
	fake_document scene{ "scene", "level" };
	edt::editor_events events;
	edt::document_manager manager{ scene, events };
	manager.adopt(std::make_unique<fake_document>("data", "same"));
	manager.adopt(std::make_unique<fake_document>("data", "same", true));
	edt::document_tabs_bar bar;
	gui::theme_semantic_colors colors;
	int activations = 0;
	const edt::document_tabs_bar::actions on{ [&](edt::document_id) { ++activations; }, [](edt::document_id) {} };

	for (int i = 0; i < 3; ++i) {
		frame([&] { bar.draw(edt::tabs_of(manager), colors, on); });
	}

	BOOST_TEST(activations == 0);
}

BOOST_FIXTURE_TEST_CASE(TheStrip_WithoutTabs_DrawsNothing, imgui_frame)
{
	edt::document_tabs_bar bar;
	gui::theme_semantic_colors colors;

	frame([&] { bar.draw({}, colors, {}); });

	BOOST_TEST(ImGui::GetCurrentContext()->CurrentTabBar == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
