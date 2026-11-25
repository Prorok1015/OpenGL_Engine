#include <boost/test/unit_test.hpp>
#include "edt_scene_document.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <memory>
#include <string>
#include <vector>

// Links and window hosts of the document manager (#1402, EPIC-82 5.4): the model has no ImGui.

namespace {

struct stub_document final : edt::document
{
	explicit stub_document(std::string name, res::tag source = res::tag::null) : m_title(std::move(name)), m_source(std::move(source)) {}

	std::string_view kind() const override { return "stub"; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return m_dirty; }
	std::uint64_t generation() const override { return 0; }

	std::string m_title;
	res::tag m_source;
	bool m_dirty = false;
};

struct links_fixture
{
	edt::document_id link(std::string name, edt::document_id parent, std::string anchor, res::tag source = res::tag::null)
	{
		edt::open_context context;
		context.kind = edt::link_kind::instance;
		context.parent = parent;
		context.anchor = std::move(anchor);
		return documents.open_linked(std::make_unique<stub_document>(std::move(name), std::move(source)), context);
	}

	edt::document_id level() { return documents.scene().id(); }

	edt::document_id asset(std::string name, res::tag source)
	{
		edt::document& opened = documents.adopt(std::make_unique<stub_document>(std::move(name), std::move(source)));
		documents.link_existing(opened.id(), {});
		return opened.id();
	}

	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, events };
};

BOOST_FIXTURE_TEST_SUITE(EdtDocumentLinks, links_fixture)

BOOST_AUTO_TEST_CASE(Chain_LevelNodeMaterial_ReadsFromTheRootDown)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	BOOST_REQUIRE(card3 != 0 && material != 0);
	BOOST_TEST((documents.chain_of(material) == std::vector<edt::document_id>{ level(), card3, material }), boost::test_tools::per_element());
	BOOST_TEST((documents.chain_of(level()) == std::vector<edt::document_id>{ level() }), boost::test_tools::per_element());
	BOOST_TEST(documents.chain_of(9999).empty());
}

BOOST_AUTO_TEST_CASE(TwoNodesOfOneFile_AreTwoLinks_ForTheSameSource)
{
	const res::tag file = res::tag::make("materials/face.material.desc");
	const edt::document_id card3 = link("Card#3", level(), "node:3", file);
	const edt::document_id card4 = link("Card#4", level(), "node:4", file);
	BOOST_REQUIRE(card3 != 0 && card4 != 0);
	BOOST_TEST(card3 != card4);
	BOOST_TEST(documents.find_link(level(), "node:3").value() == card3);
	BOOST_TEST(documents.find_link(level(), "node:4").value() == card4);
	BOOST_TEST(!documents.find_link(level(), "node:5").has_value());
}

BOOST_AUTO_TEST_CASE(OpeningTheSameAnchorAgain_ReturnsTheLink_AndMakesNoDocument)
{
	const edt::document_id first = link("Card#3", level(), "node:3");
	std::size_t before = 0;
	documents.for_each([&before](edt::document&) { ++before; });
	BOOST_TEST(link("Card#3 again", level(), "node:3") == first);
	std::size_t after = 0;
	documents.for_each([&after](edt::document&) { ++after; });
	BOOST_TEST(after == before);
}

BOOST_AUTO_TEST_CASE(AnInstanceWhoseParentIsNotOpen_IsRefused)
{
	BOOST_TEST(link("Orphan", 9999, "node:1") == 0);
}

BOOST_AUTO_TEST_CASE(FindBySource_NeverReturnsAnInstance_ButFindsTheAsset)
{
	const res::tag file = res::tag::make("materials/face.material.desc");
	const edt::document_id card3 = link("Card#3", level(), "node:3", file);
	BOOST_TEST(documents.find_by_source(file) == nullptr);
	const edt::document_id opened = asset("Face", file);
	BOOST_TEST(documents.find_by_source(file) == documents.find(opened));
	BOOST_TEST(documents.find(card3) != nullptr);
}

BOOST_AUTO_TEST_CASE(AnAssetAndAnInstanceOfOneFile_AreOpenTogether_AndDifferInTheirContext)
{
	const res::tag file = res::tag::make("materials/face.material.desc");
	const edt::document_id opened = asset("Face", file);
	const edt::document_id instance = link("Face", level(), "node:3", file);
	BOOST_REQUIRE(opened != instance);
	BOOST_TEST(documents.link_of(opened)->kind == edt::link_kind::asset);
	BOOST_TEST(documents.link_of(instance)->kind == edt::link_kind::instance);
	BOOST_TEST((documents.chain_of(opened) == std::vector<edt::document_id>{ opened }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Host_ShowsOneLink_AndTheSameLinkIsNeverInTwoHosts)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::host_id first = documents.open_host(card3);
	BOOST_REQUIRE(first != 0);
	BOOST_TEST(documents.host_of(card3).value() == first);
	BOOST_TEST(documents.open_host(card3) == first);
	BOOST_TEST(documents.hosts().size() == 1);
	BOOST_TEST(documents.open_host(9999) == 0);
}

BOOST_AUTO_TEST_CASE(Navigation_SwitchesTheHostsLinkBackAndForward_TheOthersStayOpen)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id host = documents.open_host(card3);

	BOOST_TEST(documents.set_host_link(host, material) == host);
	BOOST_TEST(documents.host_link(host).value() == material);
	BOOST_TEST(documents.find(card3) != nullptr);
	BOOST_TEST((documents.unhosted_links() == std::vector<edt::document_id>{ card3 }), boost::test_tools::per_element());

	BOOST_TEST(documents.set_host_link(host, card3) == host);
	BOOST_TEST(documents.host_link(host).value() == card3);
}

BOOST_AUTO_TEST_CASE(Navigation_ToALinkAnotherHostShows_MovesNothingAndNamesThatHost)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id first = documents.open_host(card3);
	const edt::host_id second = documents.open_host(material);
	BOOST_REQUIRE(first != second);

	BOOST_TEST(documents.set_host_link(first, material) == second);
	BOOST_TEST(documents.host_link(first).value() == card3);
	BOOST_TEST(documents.host_link(second).value() == material);
}

BOOST_AUTO_TEST_CASE(TearOff_MakesANewHost_AndTheOldOneGoesBackToTheParent)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id first = documents.open_host(material);
	BOOST_REQUIRE(first != 0);

	const edt::host_id torn = documents.move_link(material, 0);

	BOOST_TEST(torn != 0);
	BOOST_TEST(torn != first);
	BOOST_TEST(documents.host_link(torn).value() == material);
	BOOST_TEST(documents.host_link(first).value() == card3);
	BOOST_TEST(documents.host_of(material).value() == torn);
}

BOOST_AUTO_TEST_CASE(TearOff_OfALinkNoHostShows_MakesAHostAndTouchesNoOther)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id first = documents.open_host(card3);

	const edt::host_id torn = documents.move_link(material, 0);

	BOOST_TEST(documents.host_link(torn).value() == material);
	BOOST_TEST(documents.host_link(first).value() == card3);
	BOOST_TEST(documents.hosts().size() == 2);
}

BOOST_AUTO_TEST_CASE(TearOff_WhenTheParentHasAHost_ClosesTheOldHost)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id parent_host = documents.open_host(card3);
	const edt::host_id first = documents.open_host(material);

	const edt::host_id torn = documents.move_link(material, 0);

	BOOST_TEST(!documents.host_link(first).has_value());
	BOOST_TEST(documents.host_link(parent_host).value() == card3);
	BOOST_TEST(documents.host_link(torn).value() == material);
	BOOST_TEST(documents.hosts().size() == 2);
}

BOOST_AUTO_TEST_CASE(DockBack_ClosesTheHostThatShowedTheLink_AndTheTargetShowsIt)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id target = documents.open_host(card3);
	const edt::host_id source = documents.open_host(material);

	BOOST_TEST(documents.move_link(material, target) == target);

	BOOST_TEST(!documents.host_link(source).has_value());
	BOOST_TEST(documents.hosts().size() == 1);
	BOOST_TEST(documents.host_link(target).value() == material);
	BOOST_TEST(documents.find(card3) != nullptr);
	BOOST_TEST((documents.unhosted_links() == std::vector<edt::document_id>{ card3 }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(ClosingAHost_KeepsTheDocumentAndItsUnsavedChanges)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	static_cast<stub_document*>(documents.find(card3))->m_dirty = true;
	const edt::host_id host = documents.open_host(card3);

	documents.close_host(host);

	BOOST_TEST(documents.hosts().empty());
	documents.for_each([&](edt::document& open) {
		if (open.id() == card3) BOOST_TEST(open.is_dirty());
	});
	BOOST_REQUIRE(documents.find(card3) != nullptr);
	BOOST_TEST((documents.unhosted_links() == std::vector<edt::document_id>{ card3 }), boost::test_tools::per_element());
	BOOST_TEST(documents.open_host(card3) != 0);
}

BOOST_AUTO_TEST_CASE(ClosingALink_ClosesItsDescendantsFirst_AndTheirHosts)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::document_id texture = link("Albedo", material, "field:albedo");
	const edt::host_id host = documents.open_host(texture);
	std::vector<edt::document_id> closed;
	const auto connection = events.document_closed.subscribe([&closed](edt::document_id id) { closed.push_back(id); });

	documents.close(card3);

	BOOST_TEST((closed == std::vector<edt::document_id>{ texture, material, card3 }), boost::test_tools::per_element());
	BOOST_TEST(documents.find(card3) == nullptr);
	BOOST_TEST(documents.find(material) == nullptr);
	BOOST_TEST(documents.find(texture) == nullptr);
	BOOST_TEST(!documents.host_link(host).has_value());
	BOOST_TEST(!documents.link_of(card3).has_value());
	BOOST_TEST(documents.descendants_of(card3).empty());
}

BOOST_AUTO_TEST_CASE(ClosingTheChild_LeavesTheParentOpen_AndItsHostGoesBackToTheParent)
{
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const edt::document_id material = link("Face", card3, "field:material");
	const edt::host_id host = documents.open_host(material);

	documents.close(material);

	BOOST_TEST(documents.find(card3) != nullptr);
	BOOST_TEST(documents.host_link(host).value() == card3);
}

BOOST_AUTO_TEST_CASE(LinksRevision_MovesWithEveryChange)
{
	const std::uint64_t start = documents.links_revision();
	const edt::document_id card3 = link("Card#3", level(), "node:3");
	const std::uint64_t linked = documents.links_revision();
	BOOST_TEST(linked > start);
	const edt::host_id host = documents.open_host(card3);
	const std::uint64_t hosted = documents.links_revision();
	BOOST_TEST(hosted > linked);
	documents.close_host(host);
	BOOST_TEST(documents.links_revision() > hosted);
}

BOOST_AUTO_TEST_SUITE_END()

}
