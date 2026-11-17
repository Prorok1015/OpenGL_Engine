#include <boost/test/unit_test.hpp>
#include <entt/entt.hpp>
#include "edt_camera_redirect.h"
#include "edt_game_view_logic.h"
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_node_index.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

entt::entity add_game_camera(entt::registry& reg, const char* name)
{
	const auto ent = reg.create();
	reg.emplace<scn::camera_component>(ent);
	if (name) reg.emplace<scn::name_component>(ent, std::string{ name });
	return ent;
}

entt::entity add_node_camera(entt::registry& reg, const char* name, std::vector<std::uint64_t> path)
{
	const auto ent = add_game_camera(reg, name);
	reg.emplace<scn::node_id_component>(ent, std::move(path));
	return ent;
}

// Gives every backbuffer camera its own texture, as the editor does before each frame.
void redirect(entt::registry& reg, std::string_view scope = "w0")
{
	edt::camera_redirector redirector;
	redirector.begin_frame();
	redirector.redirect(reg, scope);
	redirector.end_frame({});
}

glm::ivec2 viewport_of(entt::registry& reg, entt::entity ent)
{
	return reg.get<scn::camera_component>(ent).m_viewport.size;
}

}

BOOST_AUTO_TEST_SUITE(EditorGameViewTests)

BOOST_AUTO_TEST_CASE(CollectListsOnlyRedirectedCamerasByNodeName)
{
	entt::registry reg;
	const auto a = add_game_camera(reg, "Main");
	const auto b = add_game_camera(reg, nullptr);
	const auto editor = reg.create();
	reg.emplace<scn::camera_component>(editor).color_targets.push_back(res::tag(res::tag::memory, "__editor_camera_rt"));
	redirect(reg);

	const auto cams = edt::game_view::collect_cameras(reg);
	BOOST_REQUIRE_EQUAL(cams.size(), 2u);
	BOOST_TEST((cams[0].entity == a));
	BOOST_TEST(cams[0].name == "Main");
	BOOST_TEST((cams[1].entity == b));
	BOOST_TEST(!cams[1].name.empty());
}

BOOST_AUTO_TEST_CASE(CameraNotYetRedirectedIsNotAGameCamera)
{
	entt::registry reg;
	add_game_camera(reg, "Main");
	BOOST_TEST(edt::game_view::collect_cameras(reg).empty());
}

BOOST_AUTO_TEST_CASE(DefaultSelectionIsTheFirstCamera)
{
	entt::registry reg;
	const auto a = add_game_camera(reg, "A");
	add_game_camera(reg, "B");
	redirect(reg);

	edt::game_view::selection sel;
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	BOOST_TEST((sel.selected() == a));
	BOOST_TEST(sel.selected_index() == 0);
}

BOOST_AUTO_TEST_CASE(EmptyWorldHasNoSelection)
{
	entt::registry reg;
	edt::game_view::selection sel;
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	BOOST_TEST((sel.selected() == entt::null));
	BOOST_TEST(sel.selected_index() == -1);
}

BOOST_AUTO_TEST_CASE(PickSurvivesRefreshWhileTheCameraExists)
{
	entt::registry reg;
	add_game_camera(reg, "A");
	const auto b = add_game_camera(reg, "B");
	redirect(reg);

	edt::game_view::selection sel;
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	sel.select(1);
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	BOOST_TEST((sel.selected() == b));
	BOOST_TEST(sel.selected_index() == 1);

	sel.select(7); // out of range: ignored
	BOOST_TEST((sel.selected() == b));
}

BOOST_AUTO_TEST_CASE(DeletedSelectedCameraFallsBackToTheFirstOne)
{
	entt::registry reg;
	const auto a = add_game_camera(reg, "A");
	const auto b = add_game_camera(reg, "B");
	redirect(reg);

	edt::game_view::selection sel;
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	sel.select(1);
	reg.destroy(b);
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	BOOST_TEST((sel.selected() == a));

	reg.destroy(a);
	sel.refresh(&reg, edt::game_view::collect_cameras(reg));
	BOOST_TEST((sel.selected() == entt::null));
}

BOOST_AUTO_TEST_CASE(WorldSwitchResetsThePick)
{
	entt::registry w0;
	entt::registry w1;
	add_game_camera(w0, "A");
	add_game_camera(w0, "B");
	const auto first_of_w1 = add_game_camera(w1, "X");
	add_game_camera(w1, "Y");
	add_game_camera(w1, "Z");
	redirect(w0, "w0");
	redirect(w1, "w1");

	edt::game_view::selection sel;
	sel.refresh(&w0, edt::game_view::collect_cameras(w0));
	sel.select(1);
	// Entity ids repeat between registries, so index 1 of w0 must not leak into w1.
	sel.refresh(&w1, edt::game_view::collect_cameras(w1));
	BOOST_TEST((sel.selected() == first_of_w1));
}

BOOST_AUTO_TEST_CASE(AForgottenPickComesBackByTheNodePathOfItsCamera)
{
	edt::game_view::selection sel;
	{
		entt::registry before;
		before.emplace<scn::node_id_component>(add_game_camera(before, "A"), std::vector<std::uint64_t>{ 1 });
		before.emplace<scn::node_id_component>(add_game_camera(before, "B"), std::vector<std::uint64_t>{ 2, 5 });
		redirect(before);
		sel.refresh(&before, edt::game_view::collect_cameras(before));
		sel.select(1);
		sel.forget();
	}
	// The rebuilt world spawns its nodes in another order: the entity ids moved, the paths did not.
	entt::registry after;
	add_game_camera(after, "Unnamed");
	const auto b = add_game_camera(after, "B");
	after.emplace<scn::node_id_component>(b, std::vector<std::uint64_t>{ 2, 5 });
	after.emplace<scn::node_id_component>(add_game_camera(after, "A"), std::vector<std::uint64_t>{ 1 });
	redirect(after);
	sel.refresh(&after, edt::game_view::collect_cameras(after));
	BOOST_TEST((sel.selected() == b));

	// Only the first refresh after the forget looks for it.
	sel.select(0);
	sel.refresh(&after, edt::game_view::collect_cameras(after));
	BOOST_TEST(sel.selected_index() == 0);
}

BOOST_AUTO_TEST_CASE(AForgottenPickWhoseNodeIsGoneFallsBackToTheFirstCamera)
{
	edt::game_view::selection sel;
	entt::registry before;
	add_game_camera(before, "A");
	before.emplace<scn::node_id_component>(add_game_camera(before, "B"), std::vector<std::uint64_t>{ 2 });
	redirect(before);
	sel.refresh(&before, edt::game_view::collect_cameras(before));
	sel.select(1);
	sel.forget();

	entt::registry after;
	const auto a = add_game_camera(after, "A");
	redirect(after);
	sel.refresh(&after, edt::game_view::collect_cameras(after));
	BOOST_TEST((sel.selected() == a));
}

BOOST_AUTO_TEST_CASE(ARegistryMadeAtTheAddressOfAGoneOne_IsAnotherRegistry_AndItsPickIsFoundAgainByNode)
{
	edt::game_view::selection sel;
	std::optional<entt::registry> world;
	world.emplace();
	add_node_camera(*world, "A", { 1 });
	add_node_camera(*world, "B", { 2 });
	redirect(*world);
	sel.refresh(&*world, edt::game_view::collect_cameras(*world), "w0");
	sel.select(1);
	const entt::entity old_pick = sel.selected();
	const entt::registry* address = &*world;
	const std::uint64_t old_token = edt::game_view::registry_token(*world);

	world.reset();
	world.emplace();
	BOOST_REQUIRE(&*world == address);
	// The rebuilt world spawns its nodes in the other order, so the entity id the old pick had is now the camera of node 1.
	const auto b = add_node_camera(*world, "B", { 2 });
	const auto a = add_node_camera(*world, "A", { 1 });
	redirect(*world);
	BOOST_REQUIRE((old_pick == a));
	BOOST_TEST(edt::game_view::registry_token(*world) != old_token);

	sel.refresh(&*world, edt::game_view::collect_cameras(*world), "w0");
	BOOST_TEST((sel.selected() == b));
	BOOST_TEST(!(sel.selected() == old_pick));
}

BOOST_AUTO_TEST_CASE(ClearOfTheSameRegistry_KeepsItsToken_ButNotTheEntitiesTheOldPickHeld)
{
	edt::game_view::selection sel;
	entt::registry world;
	add_node_camera(world, "A", { 1 });
	add_node_camera(world, "B", { 2 });
	redirect(world);
	sel.refresh(&world, edt::game_view::collect_cameras(world), "w0");
	sel.select(1);
	const entt::entity old_pick = sel.selected();
	const std::uint64_t token = edt::game_view::registry_token(world);

	world.clear();
	const auto b = add_node_camera(world, "B", { 2 });
	const auto a = add_node_camera(world, "A", { 1 });
	redirect(world);
	// registry::clear() leaves the ctx, but the ids come back with a new version: neither camera is the entity of the pick.
	BOOST_TEST(edt::game_view::registry_token(world) == token);
	BOOST_TEST(!(a == old_pick));
	BOOST_TEST(!(b == old_pick));

	sel.refresh(&world, edt::game_view::collect_cameras(world), "w0");
	BOOST_TEST((sel.selected() == b));
}

BOOST_AUTO_TEST_CASE(ARebuiltWorldOfTheSameName_KeepsThePickedNode_AndFallsBackWhenTheNodeIsGone)
{
	edt::game_view::selection sel;
	entt::registry before;
	add_node_camera(before, "A", { 1 });
	add_node_camera(before, "B", { 2 });
	redirect(before);
	sel.refresh(&before, edt::game_view::collect_cameras(before), "w0");
	sel.select(1);

	// Not forgotten by anyone: the registry is simply another one.
	entt::registry after;
	add_game_camera(after, "Unnamed");
	const auto b = add_node_camera(after, "B", { 2 });
	add_node_camera(after, "A", { 1 });
	redirect(after);
	sel.refresh(&after, edt::game_view::collect_cameras(after), "w0");
	BOOST_TEST((sel.selected() == b));

	// The same registry on the next frame: nothing is looked for again.
	sel.select(0);
	sel.refresh(&after, edt::game_view::collect_cameras(after), "w0");
	BOOST_TEST(sel.selected_index() == 0);

	// Rebuilt again, and now without the node of the pick (B, node 2): the first camera.
	sel.select(1);
	entt::registry gone;
	const auto first = add_node_camera(gone, "A", { 1 });
	redirect(gone);
	sel.refresh(&gone, edt::game_view::collect_cameras(gone), "w0");
	BOOST_TEST((sel.selected() == first));
}

BOOST_AUTO_TEST_CASE(AFrameWithoutARegistry_DoesNotLoseTheNodeOfThePickForTheSameWorld)
{
	edt::game_view::selection sel;
	entt::registry before;
	add_node_camera(before, "A", { 1 });
	add_node_camera(before, "B", { 2 });
	redirect(before);
	sel.refresh(&before, edt::game_view::collect_cameras(before), "w0");
	sel.select(1);

	sel.refresh(nullptr, {}, {});
	BOOST_TEST((sel.selected() == entt::null));

	entt::registry after;
	add_node_camera(after, "A", { 1 });
	const auto b = add_node_camera(after, "B", { 2 });
	redirect(after);
	sel.refresh(&after, edt::game_view::collect_cameras(after), "w0");
	BOOST_TEST((sel.selected() == b));
}

BOOST_AUTO_TEST_CASE(AnotherWorldName_StartsOver_EvenWhenItsCamerasHaveTheSameNodes)
{
	edt::game_view::selection sel;
	entt::registry w0;
	add_node_camera(w0, "A", { 1 });
	add_node_camera(w0, "B", { 2 });
	redirect(w0, "w0");
	sel.refresh(&w0, edt::game_view::collect_cameras(w0), "w0");
	sel.select(1);

	entt::registry w1;
	const auto first = add_node_camera(w1, "A", { 1 });
	add_node_camera(w1, "B", { 2 });
	redirect(w1, "w1");
	sel.refresh(&w1, edt::game_view::collect_cameras(w1), "w1");
	BOOST_TEST((sel.selected() == first));
}

BOOST_AUTO_TEST_CASE(TheRegistryToken_IsStableForOneRegistryAndNewForAnother)
{
	entt::registry one;
	entt::registry other;
	const std::uint64_t first = edt::game_view::registry_token(one);
	BOOST_TEST(first != 0u);
	BOOST_TEST(edt::game_view::registry_token(one) == first);
	BOOST_TEST(edt::game_view::registry_token(other) != first);
}

BOOST_AUTO_TEST_CASE(EveryCameraOfThePickedTargetGetsTheViewportAcrossWorlds)
{
	entt::registry world_3d;
	entt::registry overlay;
	entt::registry play_world;
	const auto cam_3d = add_game_camera(world_3d, "Game Camera");
	const auto cam_overlay = add_game_camera(overlay, "Overlay Camera");
	const auto other_set = add_game_camera(play_world, "Other");
	// the two worlds of one set share a texture; the camera of another set belongs to another picture
	edt::camera_redirector redirector;
	redirector.begin_frame();
	redirector.redirect(world_3d, "level");
	redirector.redirect(overlay, "level");
	redirector.redirect(play_world, "play");
	redirector.end_frame({});

	const res::tag picked = world_3d.get<scn::camera_component>(cam_3d).color_targets[0];
	for (entt::registry* reg : { &world_3d, &overlay, &play_world }) {
		edt::game_view::apply_target_viewports(*reg, picked, { 800, 450 });
	}
	BOOST_TEST((viewport_of(world_3d, cam_3d) == glm::ivec2{ 800, 450 }));
	BOOST_TEST((viewport_of(overlay, cam_overlay) == glm::ivec2{ 800, 450 }));
	BOOST_TEST((viewport_of(play_world, other_set) == glm::ivec2{ 0, 0 }));

	// no target (panel closed) or no usable size: everything is off again
	edt::game_view::apply_target_viewports(world_3d, res::tag{}, { 800, 450 });
	edt::game_view::apply_target_viewports(overlay, picked, { 0, 450 });
	BOOST_TEST((viewport_of(world_3d, cam_3d) == glm::ivec2{ 0, 0 }));
	BOOST_TEST((viewport_of(overlay, cam_overlay) == glm::ivec2{ 0, 0 }));
}

BOOST_AUTO_TEST_CASE(OnlyThePickedCameraOfItsOwnWorldGetsTheViewport)
{
	// "Game Camera" and "Cam B" of one world share one target; the picker decides which one is shown.
	entt::registry reg;
	const auto a = add_game_camera(reg, "Game Camera");
	const auto b = add_game_camera(reg, "Cam B");
	redirect(reg);
	const res::tag target = reg.get<scn::camera_component>(a).color_targets[0];
	BOOST_REQUIRE((reg.get<scn::camera_component>(b).color_targets[0] == target));

	edt::game_view::apply_target_viewports(reg, target, { 640, 360 }, b);
	BOOST_TEST((viewport_of(reg, a) == glm::ivec2{ 0, 0 }));
	BOOST_TEST((viewport_of(reg, b) == glm::ivec2{ 640, 360 }));

	// picking the other one hands the viewport over
	edt::game_view::apply_target_viewports(reg, target, { 800, 600 }, a);
	BOOST_TEST((viewport_of(reg, a) == glm::ivec2{ 800, 600 }));
	BOOST_TEST((viewport_of(reg, b) == glm::ivec2{ 0, 0 }));
}

BOOST_AUTO_TEST_CASE(PickedWorldHasOnePickedCameraAndOtherWorldsCompositeOnTheSameTarget)
{
	entt::registry world_3d;
	entt::registry overlay;
	const auto picked = add_game_camera(world_3d, "Game Camera");
	const auto not_picked = add_game_camera(world_3d, "Cam B");
	const auto overlay_a = add_game_camera(overlay, "Overlay A");
	const auto overlay_b = add_game_camera(overlay, "Overlay B");
	edt::camera_redirector redirector;
	redirector.begin_frame();
	redirector.redirect(world_3d, "level");
	redirector.redirect(overlay, "level");
	redirector.end_frame({});

	const res::tag target = world_3d.get<scn::camera_component>(picked).color_targets[0];
	// what the panel does: the picked world gets the picked entity, every other world gets none
	edt::game_view::apply_target_viewports(world_3d, target, { 800, 450 }, picked);
	edt::game_view::apply_target_viewports(overlay, target, { 800, 450 });
	BOOST_TEST((viewport_of(world_3d, picked) == glm::ivec2{ 800, 450 }));
	BOOST_TEST((viewport_of(world_3d, not_picked) == glm::ivec2{ 0, 0 }));
	BOOST_TEST((viewport_of(overlay, overlay_a) == glm::ivec2{ 800, 450 }));
	BOOST_TEST((viewport_of(overlay, overlay_b) == glm::ivec2{ 800, 450 }));
}

BOOST_AUTO_TEST_CASE(ClosingThePanelTurnsEveryGameCameraOff)
{
	entt::registry reg;
	const auto a = add_game_camera(reg, "A");
	redirect(reg);
	const res::tag target = reg.get<scn::camera_component>(a).color_targets[0];
	edt::game_view::apply_target_viewports(reg, target, { 640, 360 });
	BOOST_TEST((viewport_of(reg, a) == glm::ivec2{ 640, 360 }));

	edt::game_view::apply_target_viewports(reg, res::tag{}, {});
	BOOST_TEST((viewport_of(reg, a) == glm::ivec2{ 0, 0 }));

	edt::game_view::apply_target_viewports(reg, target, { 0, 100 }); // collapsed panel: no usable size
	BOOST_TEST((viewport_of(reg, a) == glm::ivec2{ 0, 0 }));
}

BOOST_AUTO_TEST_CASE(NonGameCamerasKeepTheirViewport)
{
	entt::registry reg;
	const auto game = add_game_camera(reg, "Game");
	const auto editor = reg.create();
	auto& editor_cam = reg.emplace<scn::camera_component>(editor);
	editor_cam.color_targets.push_back(res::tag(res::tag::memory, "__editor_camera_rt"));
	editor_cam.m_viewport.size = { 1280, 720 };
	redirect(reg);

	const res::tag target = reg.get<scn::camera_component>(game).color_targets[0];
	edt::game_view::apply_target_viewports(reg, target, { 320, 200 });
	edt::game_view::apply_target_viewports(reg, res::tag{}, {});
	BOOST_TEST((viewport_of(reg, editor) == glm::ivec2{ 1280, 720 }));
}

BOOST_AUTO_TEST_CASE(LetterboxKeepsTheAspect)
{
	using edt::game_view::fit_letterbox;
	BOOST_TEST((fit_letterbox({ 800.f, 600.f }, { 1600.f, 900.f }) == glm::vec2{ 800.f, 450.f }));
	BOOST_TEST((fit_letterbox({ 400.f, 600.f }, { 100.f, 100.f }) == glm::vec2{ 400.f, 400.f }));
	BOOST_TEST((fit_letterbox({ 0.f, 600.f }, { 100.f, 100.f }) == glm::vec2{ 0.f, 0.f }));
	BOOST_TEST((fit_letterbox({ 800.f, 600.f }, { 0.f, 0.f }) == glm::vec2{ 0.f, 0.f }));
}

BOOST_AUTO_TEST_SUITE_END()
