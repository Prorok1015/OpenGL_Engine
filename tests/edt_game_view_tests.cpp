#include <boost/test/unit_test.hpp>
#include <entt/entt.hpp>
#include "edt_camera_redirect.h"
#include "edt_game_view_logic.h"
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>

namespace {

entt::entity add_game_camera(entt::registry& reg, const char* name)
{
	const auto ent = reg.create();
	reg.emplace<scn::camera_component>(ent);
	if (name) reg.emplace<scn::name_component>(ent, std::string{ name });
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
