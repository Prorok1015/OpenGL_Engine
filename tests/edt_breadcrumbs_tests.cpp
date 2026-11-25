#include <boost/test/unit_test.hpp>
#include "edt_breadcrumbs.h"
#include "edt_link_close.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The crumbs of a window and the question of closing a chain (#1402, EPIC-82 5.4, 6, 10): no ImGui.

namespace {

struct crumb_document final : edt::document
{
	explicit crumb_document(std::string name) : m_title(std::move(name)) {}

	std::string_view kind() const override { return "stub"; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return res::tag::null; }
	bool is_dirty() const override { return m_dirty; }
	std::uint64_t generation() const override { return 0; }
	bool can_save() const override { return true; }
	edt::save_result save() override
	{
		if (m_refuse) return edt::save_result::failure("disk full");
		m_dirty = false;
		++m_saves;
		return edt::save_result::ok();
	}

	std::string m_title;
	bool m_dirty = false;
	bool m_refuse = false;
	int m_saves = 0;
};

struct crumbs_fixture
{
	edt::document_id link(std::string name, edt::document_id parent, std::string anchor)
	{
		edt::open_context context;
		context.kind = edt::link_kind::instance;
		context.parent = parent;
		context.anchor = std::move(anchor);
		return documents.open_linked(std::make_unique<crumb_document>(std::move(name)), context);
	}

	crumb_document& doc(edt::document_id id) { return *static_cast<crumb_document*>(documents.find(id)); }
	edt::document_id level() { return documents.scene().id(); }

	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, events };
};

BOOST_FIXTURE_TEST_SUITE(EdtBreadcrumbs, crumbs_fixture)

BOOST_AUTO_TEST_CASE(AnAssetWindow_NamesTheAssetBrowserThenItself)
{
	edt::document& opened = documents.adopt(std::make_unique<crumb_document>("Face"));
	documents.link_existing(opened.id(), {});
	const std::vector<edt::breadcrumb> rows = edt::breadcrumbs_of(documents, opened.id());
	BOOST_REQUIRE(rows.size() == 2);
	BOOST_TEST(rows[0].label == edt::k_asset_origin_label);
	BOOST_TEST(rows[0].id == 0);
	BOOST_TEST(!rows[0].draggable);
	BOOST_TEST(rows[1].label == "Face");
	BOOST_TEST(rows[1].current);
	BOOST_TEST(rows[1].draggable);
}

BOOST_AUTO_TEST_CASE(AnInstanceWindow_ShowsTheChainFromTheLevel_TheLastIsCurrent)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	doc(card3).m_dirty = true;

	const std::vector<edt::breadcrumb> rows = edt::breadcrumbs_of(documents, material);

	BOOST_REQUIRE(rows.size() == 3);
	BOOST_TEST(rows[0].id == level());
	BOOST_TEST(!rows[0].draggable);
	BOOST_TEST(rows[1].label == "Card#3");
	BOOST_TEST(rows[1].dirty);
	BOOST_TEST(!rows[1].current);
	BOOST_TEST(rows[2].id == material);
	BOOST_TEST(rows[2].current);
	BOOST_TEST(!rows[2].dirty);
}

BOOST_AUTO_TEST_CASE(TwoLinksWithOneTitleUnderOneParent_SayTheirAnchor)
{
	const edt::document_id first = link("Card", level(), "node:3");
	link("Card", level(), "node:4");
	const std::vector<edt::breadcrumb> rows = edt::breadcrumbs_of(documents, first);
	BOOST_REQUIRE(rows.size() == 2);
	BOOST_TEST(rows[1].label == "Card (node:3)");
}

BOOST_AUTO_TEST_CASE(LettingGoOnEmptySpace_MakesANewHost)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::host_id first = documents.open_host(card3);
	const edt::host_id torn = edt::apply_crumb_drop(documents, card3, { edt::drop_kind::empty_space, 0 });
	BOOST_TEST(torn != 0);
	BOOST_TEST(torn != first);
	BOOST_TEST(documents.host_of(card3).value() == torn);
}

BOOST_AUTO_TEST_CASE(LettingGoOnAnotherHostsBar_DocksBack_OnItsOwnBarNothingHappens)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id target = documents.open_host(card3);
	const edt::host_id source = documents.open_host(material);

	BOOST_TEST(edt::apply_crumb_drop(documents, material, { edt::drop_kind::breadcrumb_bar, source }) == 0);
	BOOST_TEST(documents.host_link(source).value() == material);

	BOOST_TEST(edt::apply_crumb_drop(documents, material, { edt::drop_kind::breadcrumb_bar, target }) == target);
	BOOST_TEST(documents.host_link(target).value() == material);
	BOOST_TEST(!documents.host_link(source).has_value());
}

BOOST_AUTO_TEST_CASE(ADropOfARootDocumentOrNothing_DoesNothing)
{
	BOOST_TEST(edt::apply_crumb_drop(documents, level(), { edt::drop_kind::empty_space, 0 }) == 0);
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	BOOST_TEST(edt::apply_crumb_drop(documents, card3, { edt::drop_kind::none, 0 }) == 0);
	BOOST_TEST(documents.hosts().empty());
}

struct close_fixture : crumbs_fixture
{
	close_fixture()
	{
		card3 = link("Card#3", level(), "node:3");
		material = link("Face", card3, "field:material");
		ports.ask = [this](const std::vector<std::string>& titles, std::function<void(edt::chain_answer)> answer) {
			++questions;
			asked = titles;
			if (reply) answer(*reply);
		};
		ports.close = [this](edt::document_id id) {
			closed.push_back(id);
			documents.close(id);
		};
	}

	edt::document_id card3 = 0;
	edt::document_id material = 0;
	edt::chain_close_ports ports;
	int questions = 0;
	std::vector<std::string> asked;
	std::optional<edt::chain_answer> reply;
	std::vector<edt::document_id> closed;
};

BOOST_FIXTURE_TEST_CASE(CloseChain_OfCleanDocuments_AsksNothingAndClosesTheDeepestFirst, close_fixture)
{
	BOOST_TEST(edt::request_close_link(documents, card3, ports));
	BOOST_TEST(questions == 0);
	BOOST_TEST((closed == std::vector<edt::document_id>{ material, card3 }), boost::test_tools::per_element());
	BOOST_TEST(documents.find(card3) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(CloseChain_WithUnsavedChanges_AsksOnceForTheWholeChain_AndNamesThem, close_fixture)
{
	doc(card3).m_dirty = true;
	doc(material).m_dirty = true;
	reply = edt::chain_answer::cancel;

	BOOST_TEST(!edt::request_close_link(documents, card3, ports));

	BOOST_TEST(questions == 1);
	BOOST_TEST((asked == std::vector<std::string>{ "Face", "Card#3" }), boost::test_tools::per_element());
	BOOST_TEST(documents.find(card3) != nullptr);
	BOOST_TEST(documents.find(material) != nullptr);
}

BOOST_FIXTURE_TEST_CASE(CloseChain_Discard_ClosesEverything, close_fixture)
{
	doc(material).m_dirty = true;
	reply = edt::chain_answer::discard;
	BOOST_TEST(edt::request_close_link(documents, card3, ports));
	BOOST_TEST(documents.find(card3) == nullptr);
	BOOST_TEST(documents.find(material) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(CloseChain_Save_WritesTheDirtyOnesThenCloses, close_fixture)
{
	doc(card3).m_dirty = true;
	doc(material).m_dirty = true;
	reply = edt::chain_answer::save;
	int saved_before_close = 0;
	ports.close = [&](edt::document_id id) {
		if (id == material) saved_before_close = doc(card3).m_saves + doc(material).m_saves;
		documents.close(id);
	};
	BOOST_TEST(edt::request_close_link(documents, card3, ports));
	BOOST_TEST(saved_before_close == 2);
	BOOST_TEST(documents.find(card3) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(CloseChain_ASaveThatFails_ClosesNothing_AndSaysWhy, close_fixture)
{
	doc(material).m_dirty = true;
	doc(material).m_refuse = true;
	reply = edt::chain_answer::save;
	std::string said;
	BOOST_TEST(!edt::request_close_link(documents, card3, ports, [&said](const std::string& why) { said = why; }));
	BOOST_TEST(said.find("disk full") != std::string::npos);
	BOOST_TEST(said.find("Face") != std::string::npos);
	BOOST_TEST(documents.find(card3) != nullptr);
	BOOST_TEST(documents.find(material) != nullptr);
}

BOOST_FIXTURE_TEST_CASE(ClosingAHost_AsksNothing_AndKeepsTheUnsavedChange, close_fixture)
{
	doc(card3).m_dirty = true;
	const edt::host_id host = documents.open_host(card3);
	documents.close_host(host);
	BOOST_TEST(questions == 0);
	BOOST_TEST(doc(card3).m_dirty);
}

BOOST_AUTO_TEST_SUITE_END()

}
