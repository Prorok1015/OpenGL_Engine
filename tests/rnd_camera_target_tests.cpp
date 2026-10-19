#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <vector>
#include <entt/entt.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/scn_camera_component.hpp>
#include "edt_camera_redirect.h"

BOOST_AUTO_TEST_SUITE(RndCameraTargetTests)

BOOST_AUTO_TEST_CASE(CameraWithoutColorTargetDrawsToBackbuffer) {
	rnd::camera_render_data_t cam;
	const auto target = rnd::select_camera_target(cam);
	BOOST_TEST((target.kind == rnd::render_target_kind::backbuffer));
}

BOOST_AUTO_TEST_CASE(InvalidFirstColorTargetCountsAsBackbuffer) {
	rnd::camera_render_data_t cam;
	cam.color_targets.push_back(res::tag{});
	BOOST_TEST(rnd::targets_backbuffer(cam.color_targets));
}

BOOST_AUTO_TEST_CASE(CameraWithColorTargetDrawsToThatTexture) {
	rnd::camera_render_data_t cam;
	const res::tag rt(res::tag::memory, "__test_rt");
	cam.color_targets.push_back(rt);
	const auto target = rnd::select_camera_target(cam);
	BOOST_TEST((target.kind == rnd::render_target_kind::texture));
	BOOST_TEST((target.texture == rt));
}

BOOST_AUTO_TEST_CASE(OwnViewportWinsOverBackbufferSize) {
	const auto vp = rnd::effective_camera_viewport({ 10, 20, 300, 200 }, true, { 1920, 1080 });
	BOOST_REQUIRE(vp.has_value());
	BOOST_TEST((*vp == glm::ivec4{ 10, 20, 300, 200 }));
}

BOOST_AUTO_TEST_CASE(BackbufferCameraWithoutViewportCoversTheBackbuffer) {
	const auto vp = rnd::effective_camera_viewport({ 0, 0, 0, 0 }, true, { 1920, 1080 });
	BOOST_REQUIRE(vp.has_value());
	BOOST_TEST((*vp == glm::ivec4{ 0, 0, 1920, 1080 }));
}

BOOST_AUTO_TEST_CASE(TextureCameraWithoutViewportDoesNotRender) {
	BOOST_TEST(!rnd::effective_camera_viewport({ 0, 0, 0, 0 }, false, { 1920, 1080 }).has_value());
}

BOOST_AUTO_TEST_CASE(UnknownBackbufferSizeMeansNothingToRender) {
	BOOST_TEST(!rnd::effective_camera_viewport({ 0, 0, 0, 0 }, true, { 0, 0 }).has_value());
}

BOOST_AUTO_TEST_CASE(EditorRedirectsBackbufferCamerasToTextures) {
	entt::registry reg;
	const auto game_cam = reg.create();
	reg.emplace<scn::camera_component>(game_cam);
	const auto editor_cam = reg.create();
	auto& editor_comp = reg.emplace<scn::camera_component>(editor_cam);
	const res::tag editor_rt(res::tag::memory, "__editor_camera_rt");
	editor_comp.color_targets.push_back(editor_rt);

	edt::camera_redirector redirector;
	redirector.begin_frame();
	BOOST_TEST(redirector.redirect(reg, "w0") == 1u);
	redirector.end_frame({});

	const auto& redirected = reg.get<scn::camera_component>(game_cam);
	BOOST_REQUIRE(redirected.color_targets.size() == 1u);
	BOOST_TEST(!rnd::targets_backbuffer(redirected.color_targets));
	BOOST_TEST((reg.get<scn::camera_component>(editor_cam).color_targets[0] == editor_rt));
	BOOST_TEST(redirector.tracked_count() == 1u); // the editor camera's own target is not ours

	// Already redirected: nothing left to do.
	redirector.begin_frame();
	BOOST_TEST(redirector.redirect(reg, "w0") == 0u);
	redirector.end_frame({});
	BOOST_TEST(redirector.tracked_count() == 1u);
}

BOOST_AUTO_TEST_CASE(RedirectedTargetsAreSharedWithinAScopeAndDifferBetweenScopes) {
	entt::registry a;
	entt::registry b;
	const auto a1 = a.create();
	const auto a2 = a.create();
	const auto b1 = b.create();
	a.emplace<scn::camera_component>(a1);
	a.emplace<scn::camera_component>(a2);
	b.emplace<scn::camera_component>(b1);
	edt::camera_redirector redirector;
	redirector.begin_frame();
	redirector.redirect(a, "set0");
	redirector.redirect(b, "set1");
	redirector.end_frame({});

	const auto& t_a1 = a.get<scn::camera_component>(a1).color_targets[0];
	const auto& t_a2 = a.get<scn::camera_component>(a2).color_targets[0];
	const auto& t_b1 = b.get<scn::camera_component>(b1).color_targets[0];
	BOOST_TEST((t_a1 == t_a2)); // layers of one picture
	BOOST_TEST((t_a1 != t_b1)); // another set is another picture
	BOOST_TEST(redirector.tracked_count() == 2u);
}

BOOST_AUTO_TEST_CASE(TwoWorldsOfOneScopeDrawIntoOneTarget) {
	entt::registry world_3d;
	entt::registry overlay;
	const auto cam_3d = world_3d.create();
	const auto cam_overlay = overlay.create();
	world_3d.emplace<scn::camera_component>(cam_3d);
	auto& overlay_cam = overlay.emplace<scn::camera_component>(cam_overlay);
	overlay_cam.layer = 1;
	overlay_cam.clear = scn::camera_clear::depth;

	edt::camera_redirector redirector;
	redirector.begin_frame();
	BOOST_TEST(redirector.redirect(world_3d, "level") == 1u);
	BOOST_TEST(redirector.redirect(overlay, "level") == 1u);
	redirector.end_frame({});

	const auto& t_3d = world_3d.get<scn::camera_component>(cam_3d).color_targets;
	const auto& t_overlay = overlay.get<scn::camera_component>(cam_overlay).color_targets;
	BOOST_REQUIRE(!t_3d.empty() && !t_overlay.empty());
	BOOST_TEST((t_3d[0] == t_overlay[0]));
	BOOST_TEST(edt::is_game_camera_target(t_3d[0]));
	BOOST_TEST(redirector.tracked_count() == 1u);
}

BOOST_AUTO_TEST_CASE(CamerasWithDifferentViewportsGetTheirOwnTargets) {
	entt::registry reg;
	const auto full = reg.create();
	const auto corner = reg.create();
	const auto corner2 = reg.create();
	reg.emplace<scn::camera_component>(full);
	reg.emplace<scn::camera_component>(corner).m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 320, 180 } };
	reg.emplace<scn::camera_component>(corner2).m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 320, 180 } };

	edt::camera_redirector redirector;
	redirector.begin_frame();
	redirector.redirect(reg, "level");
	redirector.end_frame({});

	const auto target_of = [&](entt::entity e) { return reg.get<scn::camera_component>(e).color_targets[0]; };
	BOOST_TEST((target_of(full) != target_of(corner)));
	BOOST_TEST((target_of(corner) == target_of(corner2)));
	BOOST_TEST(redirector.tracked_count() == 2u);
}

BOOST_AUTO_TEST_CASE(TargetsOfDestroyedCamerasAreReleasedAndCountStaysBounded) {
	entt::registry reg;
	edt::camera_redirector redirector;
	std::vector<res::tag> released;
	const auto release = [&released](const res::tag& tag) { released.push_back(tag); };

	std::size_t max_tracked = 0;
	for (int i = 0; i < 50; ++i) {
		const auto cam = reg.create();
		reg.emplace<scn::camera_component>(cam);
		redirector.begin_frame();
		redirector.redirect(reg, "w0");
		redirector.end_frame(release);
		max_tracked = std::max(max_tracked, redirector.tracked_count());
		reg.destroy(cam);
	}
	BOOST_TEST(max_tracked == 1u);

	// The last camera is gone too: one more frame releases its target.
	redirector.begin_frame();
	redirector.redirect(reg, "w0");
	redirector.end_frame(release);
	BOOST_TEST(redirector.tracked_count() == 0u);
	// Entity slots are recycled, so all 50 cameras shared one tag; it is released exactly once, when
	// the last of them is gone.
	BOOST_TEST(released.size() == 1u);
}

BOOST_AUTO_TEST_CASE(OnlyTheTargetOfADestroyedCameraIsReleased) {
	entt::registry reg;
	const auto keep = reg.create();
	const auto drop = reg.create();
	reg.emplace<scn::camera_component>(keep);
	// a viewport of its own gives `drop` a target of its own; the cameras of one viewport share theirs
	reg.emplace<scn::camera_component>(drop).m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 64, 64 } };
	edt::camera_redirector redirector;
	std::vector<res::tag> released;
	const auto release = [&released](const res::tag& tag) { released.push_back(tag); };

	redirector.begin_frame();
	redirector.redirect(reg, "w0");
	redirector.end_frame(release);
	const res::tag dropped_tag = reg.get<scn::camera_component>(drop).color_targets[0];
	reg.destroy(drop);

	redirector.begin_frame();
	redirector.redirect(reg, "w0");
	redirector.end_frame(release);
	BOOST_REQUIRE(released.size() == 1u);
	BOOST_TEST((released[0] == dropped_tag));
	BOOST_TEST(redirector.tracked_count() == 1u);
}

BOOST_AUTO_TEST_CASE(SharedTargetStaysAliveWhileOneCameraOfItRemains) {
	entt::registry reg;
	const auto keep = reg.create();
	const auto drop = reg.create();
	reg.emplace<scn::camera_component>(keep);
	reg.emplace<scn::camera_component>(drop);
	edt::camera_redirector redirector;
	std::vector<res::tag> released;
	const auto release = [&released](const res::tag& tag) { released.push_back(tag); };

	redirector.begin_frame();
	redirector.redirect(reg, "w0");
	redirector.end_frame(release);
	const res::tag shared = reg.get<scn::camera_component>(keep).color_targets[0];
	BOOST_REQUIRE((reg.get<scn::camera_component>(drop).color_targets[0] == shared));
	reg.destroy(drop);

	// the other camera still draws into the target: it must not be released
	redirector.begin_frame();
	redirector.redirect(reg, "w0");
	redirector.end_frame(release);
	BOOST_TEST(released.empty());
	BOOST_TEST(redirector.tracked_count() == 1u);

	// once the last camera is gone, the target is released
	reg.destroy(keep);
	redirector.begin_frame();
	redirector.redirect(reg, "w0");
	redirector.end_frame(release);
	BOOST_REQUIRE(released.size() == 1u);
	BOOST_TEST((released[0] == shared));
}

BOOST_AUTO_TEST_SUITE_END()
