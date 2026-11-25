#include <boost/test/unit_test.hpp>
#include "edt_preview_fixture.hpp"
#include "edt_preview_stage.h"
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

// The reusable preview stage (#1196): its preview set and orbit camera, parking, the sky choice, and what is given up on
// a layer change, on close and when its owner goes. The content is a trivial one: an empty node. No pixels.

namespace {

struct probe_content final : edt::preview_content
{
	state update() override { return next; }
	void build(scn::prefab_desc::prefab_node& root) override
	{
		++builds;
		root.name = "probe";
		scn::prefab_desc::prefab_node node;
		node.name = "probe_node";
		node.node_id = 0x5eed;
		root.children.push_back(std::move(node));
	}
	void refresh(scn::prefab_desc::prefab_node&) override { ++refreshes; }
	void release() override { ++releases; }
	edt::preview_framing framing() const override { return frame; }

	state next = state::changed;
	edt::preview_framing frame;
	int builds = 0;
	int refreshes = 0;
	int releases = 0;
};

struct preview_stage_fixture : test_util::preview_env
{
	preview_stage_fixture()
		: preview_env("edt_preview_stage")
	{
	}

	~preview_stage_fixture()
	{
		stage.reset();
	}

	// A stage over the probe, updated once (one frame shown).
	edt::preview_stage& open()
	{
		stage.emplace(services, content);
		stage->set_targets_release(record_released());
		stage->update();
		return *stage;
	}

	// The orbit controller of the stage's camera; null: none.
	scn::mouse_controller_component* orbit()
	{
		scn::camera_component* camera = stage ? stage->camera() : nullptr;
		if (!camera) return nullptr;
		entt::registry& reg = *stage->registry();
		for (const entt::entity entity : reg.view<scn::camera_component, scn::mouse_controller_component>()) {
			if (&reg.get<scn::camera_component>(entity) == camera) return &reg.get<scn::mouse_controller_component>(entity);
		}
		return nullptr;
	}

	probe_content content;
	std::optional<edt::preview_stage> stage;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtPreviewStageTests, preview_stage_fixture)

BOOST_AUTO_TEST_CASE(Open_MakesThePreviewSet_AndAnOrbitCameraWithItsTargets)
{
	edt::preview_stage& shown = open();

	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(content.builds == 1);
	BOOST_REQUIRE(!shown.set_name().empty());
	BOOST_TEST(mgr->find_set(shown.set_name()) != nullptr);
	BOOST_TEST(shown.world_template().view().starts_with("memory://editor/__preview_stage/"));
	scn::camera_component* camera = shown.camera();
	BOOST_REQUIRE(camera != nullptr);
	BOOST_TEST(shown.render_target().starts_with("memory://__preview_camera_rt/"));
	BOOST_TEST(camera->color_targets.front().view() == shown.render_target());
	BOOST_TEST(camera->depth_target.is_valid());
	BOOST_TEST(camera->tp_accum_target.is_valid());
	BOOST_TEST(camera->tp_reveal_target.is_valid());
	entt::registry& reg = *shown.registry();
	std::size_t orbit = 0;
	for (const entt::entity entity : reg.view<scn::camera_component, scn::mouse_controller_component>()) {
		if (&reg.get<scn::camera_component>(entity) == camera) ++orbit;
	}
	BOOST_TEST(orbit == 1u);

	// A changed content is reconciled, an unchanged one is not.
	shown.update();
	BOOST_TEST(content.refreshes == 1);
	content.next = probe_content::state::unchanged;
	shown.update();
	BOOST_TEST(content.refreshes == 1);
	BOOST_TEST(content.builds == 1);
}

BOOST_AUTO_TEST_CASE(Park_TheCameraDrawsNothing)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.camera() != nullptr);

	shown.set_viewport_size(64, 48);
	BOOST_TEST(shown.camera()->m_viewport.size.x == 64);
	BOOST_TEST(shown.camera()->m_viewport.size.y == 48);
	shown.park();
	BOOST_TEST(shown.camera()->m_viewport.size.x == 0);
	BOOST_TEST(shown.camera()->m_viewport.size.y == 0);
}

BOOST_AUTO_TEST_CASE(TheSkyToggle_HidesTheTemplatesSky_AndSurvivesARebuild)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(shown.sky());
	BOOST_TEST((skies(shown) == std::pair<std::size_t, std::size_t>{ 1u, 1u }));

	shown.set_sky(false);
	BOOST_TEST((skies(shown) == std::pair<std::size_t, std::size_t>{ 1u, 0u }));
	shown.set_sky(true);
	BOOST_TEST((skies(shown) == std::pair<std::size_t, std::size_t>{ 1u, 1u }));

	// A world made again keeps the choice.
	shown.set_sky(false);
	shown.release_world();
	shown.rebuild_world();
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST((skies(shown) == std::pair<std::size_t, std::size_t>{ 1u, 0u }));
}

BOOST_AUTO_TEST_CASE(ALayerChange_DropsTheSet_AndMakesItAgain)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.has_world());
	const std::string set = shown.set_name();

	shown.release_world();
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(mgr->find_set(set) == nullptr);
	BOOST_TEST(shown.registry() == nullptr);
	BOOST_TEST(released.empty());
	BOOST_TEST(content.releases == 0);
	// Hidden frames between the two halves of the change make nothing in the old layer.
	shown.update();
	BOOST_TEST(!shown.has_world());

	shown.rebuild_world();
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(mgr->find_set(set) != nullptr);
	BOOST_TEST(shown.camera() != nullptr);
	BOOST_TEST(content.builds == 2);
}

BOOST_AUTO_TEST_CASE(Close_GivesUpTheTargets_TheTemplateCopy_AndTheContent)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.has_world());
	const std::string set = shown.set_name();
	const std::string target = shown.render_target();
	const res::tag world = shown.world_template();
	BOOST_TEST(ram->files.contains(world));

	shown.close();
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(mgr->find_set(set) == nullptr);
	BOOST_REQUIRE(released.size() == 4u);
	BOOST_TEST(released.front().view() == target);
	BOOST_TEST(!ram->files.contains(world));
	BOOST_TEST(content.releases >= 1);
}

BOOST_AUTO_TEST_CASE(DestroyingTheStage_LeavesNoSetAndNoTargets)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.has_world());
	const std::string set = shown.set_name();
	const res::tag world = shown.world_template();

	// The owner window goes.
	stage.reset();
	BOOST_TEST(mgr->find_set(set) == nullptr);
	BOOST_TEST(released.size() == 4u);
	BOOST_TEST(!ram->files.contains(world));
	BOOST_TEST(content.releases >= 1);
}

BOOST_AUTO_TEST_CASE(AContentWithNothingToShow_ClosesTheStage)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.has_world());

	content.next = probe_content::state::none;
	shown.update();
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(released.size() == 4u);
	BOOST_TEST(content.releases >= 1);
}

BOOST_AUTO_TEST_CASE(TheContentsFraming_PlacesTheCamera_AndSyncAppliesItAgain)
{
	content.frame.distance = 5.f;
	content.frame.pitch_degrees = -60.f;
	open();
	scn::mouse_controller_component* ctrl = orbit();
	BOOST_REQUIRE(ctrl != nullptr);
	BOOST_TEST(ctrl->distance == 5.f);
	BOOST_TEST(ctrl->rotation.x == glm::radians(-60.f));

	// The user orbits; an edit keeps it, a choice in the owner's UI frames the camera again.
	ctrl->distance = 9.f;
	content.next = probe_content::state::changed;
	stage->update();
	BOOST_TEST(orbit()->distance == 9.f);
	content.frame.distance = 7.f;
	stage->sync();
	BOOST_TEST(orbit()->distance == 7.f);
}

BOOST_AUTO_TEST_CASE(AFailedOpen_IsTriedAgainAfterClose)
{
	// No context manager: the world cannot be made.
	services.ctx_manager.reset();
	edt::preview_stage& shown = open();
	BOOST_TEST(!shown.has_world());
	services.ctx_manager = mgr;
	shown.update();
	BOOST_TEST(!shown.has_world());

	shown.close();
	shown.update();
	BOOST_TEST(shown.has_world());
}

BOOST_AUTO_TEST_CASE(ACloseDuringALayerChange_OpensNothingInTheOldLayer)
{
	edt::preview_stage& shown = open();
	BOOST_REQUIRE(shown.has_world());
	shown.release_world();

	content.next = probe_content::state::none;
	shown.update();
	content.next = probe_content::state::changed;
	shown.update();
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(content.builds == 1);

	shown.rebuild_world();
	BOOST_TEST(shown.has_world());
	BOOST_TEST(content.builds == 2);
}

BOOST_AUTO_TEST_SUITE_END()
