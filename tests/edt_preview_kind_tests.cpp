#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
#include <memory>
#include <string>
#include <vector>

// The preview of an asset kind (#589): the document_manager asks the asset kinds which one previews a source, keeps one
// document per kind, and closes and destroys it when its kind is removed, with a warning when it had unsaved changes.

namespace {

struct preview_counters
{
	int made = 0;
	int destroyed = 0;
	int closed = 0;
};

class plain_document : public edt::document
{
public:
	std::string_view kind() const override { return "plain"; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return false; }
	std::uint64_t generation() const override { return 0; }

protected:
	std::string m_title = "plain";
	res::tag m_source;
};

class fake_preview final : public edt::previewable_document
{
public:
	fake_preview(std::string label, preview_counters& counters, bool& dirty)
		: m_label(std::move(label)), m_counters(counters), m_dirty(dirty)
	{
		++m_counters.made;
	}
	~fake_preview() override { ++m_counters.destroyed; }

	std::string_view kind() const override { return m_label; }
	const std::string& title() const override { return m_label_text; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return m_dirty; }
	std::uint64_t generation() const override { return 0; }

	void open(const res::tag& source) override
	{
		m_source = source;
		m_label_text = std::string(source.relative());
		m_open = source.relative().find("missing") == std::string_view::npos;
	}
	void close() override
	{
		m_open = false;
		++m_counters.closed;
	}
	bool is_open() const override { return m_open; }

private:
	std::string m_label;
	std::string m_label_text;
	preview_counters& m_counters;
	bool& m_dirty;
	res::tag m_source;
	bool m_open = false;
};

struct preview_fixture
{
	edt::asset_preview_desc preview_kind(std::string id, std::string suffix, preview_counters& counters)
	{
		edt::asset_preview_desc preview;
		preview.id = id;
		preview.suffix = std::move(suffix);
		preview.make_preview = [id, &counters, this](edt::editor_services&) -> std::unique_ptr<edt::previewable_document> {
			return std::make_unique<fake_preview>(id, counters, dirty);
		};
		return preview;
	}

	std::size_t closed_events() const { return closed_ids.size(); }

	plain_document scene;
	edt::editor_events events;
	edt::editor_services services;
	edt::problem_list problems{ &events };
	edt::document_manager documents{ scene, events };
	edt::asset_kind_registry kinds;
	preview_counters prefab_counters;
	preview_counters model_counters;
	bool dirty = false;
	std::vector<edt::document_id> closed_ids;
	edt::connection closed_connection = events.document_closed.subscribe([this](edt::document_id id) { closed_ids.push_back(id); });
};

}

BOOST_FIXTURE_TEST_SUITE(EdtPreviewKindTests, preview_fixture)

BOOST_AUTO_TEST_CASE(APreview_IsRegisteredApartFromTheKinds_AndOneWithoutAnIdOrAFunctionIsRefused)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	BOOST_CHECK(prefab.active());
	BOOST_CHECK(kinds.find_preview("prefab") != nullptr);
	BOOST_CHECK_EQUAL(kinds.size(), 0u);

	edt::asset_preview_desc no_function;
	no_function.id = "no_function";
	BOOST_CHECK(!kinds.add_preview(std::move(no_function)).active());

	auto no_id = preview_kind("", ".x", prefab_counters);
	BOOST_CHECK(!kinds.add_preview(std::move(no_id)).active());

	auto taken = kinds.add_preview(preview_kind("prefab", ".other", model_counters));
	BOOST_CHECK(!taken.active());
	BOOST_CHECK(kinds.preview_for("a.prefab.desc") != nullptr);
	BOOST_CHECK(kinds.preview_for("a.other") == nullptr);
}

BOOST_AUTO_TEST_CASE(ThePreviewWithTheLongestMatchingSuffix_MakesTheDocument_AndNoneMeansNull)
{
	auto fallback = kinds.add_preview(preview_kind("any", "", model_counters));
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	documents.set_preview_kinds(kinds, services, &problems);

	edt::previewable_document* preview = documents.open_preview(res::tag::make("prefabs/a.prefab.desc"));
	BOOST_REQUIRE(preview != nullptr);
	BOOST_CHECK_EQUAL(preview->kind(), "prefab");
	BOOST_CHECK_EQUAL(prefab_counters.made, 1);
	BOOST_CHECK_EQUAL(model_counters.made, 0);

	preview = documents.open_preview(res::tag::make("models/b.glb"));
	BOOST_REQUIRE(preview != nullptr);
	BOOST_CHECK_EQUAL(preview->kind(), "any");

	fallback = edt::registration{};
	prefab = edt::registration{};
	BOOST_CHECK(documents.open_preview(res::tag::make("models/c.glb")) == nullptr);
}

BOOST_AUTO_TEST_CASE(AKindMakesItsPreviewOnce_AndThePreviousOneClosesWhenAnotherOpens)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	auto model = kinds.add_preview(preview_kind("model", ".glb", model_counters));
	documents.set_preview_kinds(kinds, services, &problems);

	documents.open_preview(res::tag::make("a.prefab.desc"));
	documents.open_preview(res::tag::make("b.prefab.desc"));
	BOOST_CHECK_EQUAL(prefab_counters.made, 1);
	BOOST_CHECK_EQUAL(prefab_counters.closed, 1);

	documents.open_preview(res::tag::make("c.glb"));
	BOOST_CHECK_EQUAL(model_counters.made, 1);
	BOOST_CHECK_EQUAL(prefab_counters.closed, 2);
	BOOST_REQUIRE(documents.preview() != nullptr);
	BOOST_CHECK_EQUAL(documents.preview()->kind(), "model");
}

BOOST_AUTO_TEST_CASE(AnUnopenableSource_GivesADocumentThatIsNotOpen_AndAnnouncesNothing)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	documents.set_preview_kinds(kinds, services, &problems);
	edt::previewable_document* preview = documents.open_preview(res::tag::make("missing.prefab.desc"));
	BOOST_REQUIRE(preview != nullptr);
	BOOST_CHECK(!preview->is_open());
	BOOST_CHECK(documents.preview() == nullptr);
}

BOOST_AUTO_TEST_CASE(RemovingThePreview_ClosesAndDestroysItsOpenPreview_AndTheSceneStaysTheActiveDocument)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	documents.set_preview_kinds(kinds, services, &problems);
	documents.open_preview(res::tag::make("a.prefab.desc"));
	BOOST_REQUIRE(documents.preview() != nullptr);

	prefab = edt::registration{};
	BOOST_CHECK(documents.preview() == nullptr);
	BOOST_CHECK_EQUAL(prefab_counters.closed, 1);
	BOOST_CHECK_EQUAL(prefab_counters.destroyed, 1);
	BOOST_CHECK_EQUAL(closed_events(), 1u);
	BOOST_CHECK(documents.active() == &scene);
	BOOST_CHECK(problems.sources().empty());
}

BOOST_AUTO_TEST_CASE(RemovingThePreview_OfAPreviewWithUnsavedChanges_ClosesItAndSaysSoInTheProblemsAndTheLog)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	documents.set_preview_kinds(kinds, services, &problems);
	documents.open_preview(res::tag::make("a.prefab.desc"));
	dirty = true;

	prefab = edt::registration{};
	BOOST_CHECK(documents.preview() == nullptr);
	BOOST_CHECK_EQUAL(prefab_counters.destroyed, 1);
	const auto listed = problems.list();
	BOOST_REQUIRE_EQUAL(listed.size(), 1u);
	BOOST_CHECK_EQUAL(listed.front().source, "preview");
	BOOST_CHECK(listed.front().item.severity == edt::problem_severity::warning);
	BOOST_CHECK_MESSAGE(listed.front().item.message.find("unsaved changes") != std::string::npos, listed.front().item.message);
	BOOST_CHECK_EQUAL(listed.front().item.file, "a.prefab.desc");

	// The next preview that opens clears the warning.
	auto again = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	dirty = false;
	documents.open_preview(res::tag::make("b.prefab.desc"));
	BOOST_CHECK(problems.sources().empty());
}

BOOST_AUTO_TEST_CASE(APreviewCanBeOpenedByItsId_WhateverASpecificerSuffixSays)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	auto module_preview = kinds.add_preview(preview_kind("module", ".ui.prefab.desc", model_counters));
	documents.set_preview_kinds(kinds, services, &problems);

	BOOST_REQUIRE(documents.open_preview(res::tag::make("a.ui.prefab.desc")) != nullptr);
	BOOST_CHECK_EQUAL(documents.preview()->kind(), "module");
	edt::previewable_document* by_id = documents.open_preview("prefab", res::tag::make("a.ui.prefab.desc"));
	BOOST_REQUIRE(by_id != nullptr);
	BOOST_CHECK_EQUAL(by_id->kind(), "prefab");
	BOOST_CHECK(documents.open_preview("no_such_preview", res::tag::make("a.prefab.desc")) == nullptr);
}

BOOST_AUTO_TEST_CASE(AListenerOfTheClose_MayOpenAnotherPreview_WhileTheRemovedOneIsBeingDropped)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	auto model = kinds.add_preview(preview_kind("model", ".glb", model_counters));
	documents.set_preview_kinds(kinds, services, &problems);
	documents.open_preview(res::tag::make("a.prefab.desc"));
	bool reopened = false;
	auto listener = events.document_closed.subscribe([&](edt::document_id) {
		if (reopened) return;
		reopened = true;
		documents.open_preview(res::tag::make("b.glb"));
	});

	prefab = edt::registration{};
	BOOST_CHECK(reopened);
	BOOST_CHECK_EQUAL(prefab_counters.destroyed, 1);
	BOOST_REQUIRE(documents.preview() != nullptr);
	BOOST_CHECK_EQUAL(documents.preview()->kind(), "model");
}

BOOST_AUTO_TEST_CASE(RemovingAnotherPreview_LeavesTheOpenPreviewAlone)
{
	auto prefab = kinds.add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	auto model = kinds.add_preview(preview_kind("model", ".glb", model_counters));
	documents.set_preview_kinds(kinds, services, &problems);
	documents.open_preview(res::tag::make("a.prefab.desc"));
	documents.open_preview(res::tag::make("b.glb"));

	prefab = edt::registration{};
	BOOST_CHECK_EQUAL(prefab_counters.destroyed, 1);
	BOOST_REQUIRE(documents.preview() != nullptr);
	BOOST_CHECK_EQUAL(documents.preview()->kind(), "model");
	BOOST_CHECK_EQUAL(model_counters.destroyed, 0);
}

BOOST_AUTO_TEST_CASE(ARegistryThatIsGoneFirst_LeavesTheManagerSafe)
{
	auto registry = std::make_unique<edt::asset_kind_registry>();
	auto prefab = registry->add_preview(preview_kind("prefab", ".prefab.desc", prefab_counters));
	documents.set_preview_kinds(*registry, services, &problems);
	documents.open_preview(res::tag::make("a.prefab.desc"));
	documents.shutdown();
	prefab = edt::registration{};
	registry.reset();
	BOOST_CHECK_EQUAL(prefab_counters.made, 1);
}

BOOST_AUTO_TEST_SUITE_END()
