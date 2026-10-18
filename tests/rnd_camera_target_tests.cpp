#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <vector>
#include <entt/entt.hpp>
#include "rnd_render_packet.hpp"
#include "scn_camera_component.hpp"
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

BOOST_AUTO_TEST_CASE(RedirectedTargetsDifferBetweenCamerasAndWorlds) {
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
	redirector.redirect(a, "w0");
	redirector.redirect(b, "w1");
	redirector.end_frame({});

	const auto& t_a1 = a.get<scn::camera_component>(a1).color_targets[0];
	const auto& t_a2 = a.get<scn::camera_component>(a2).color_targets[0];
	const auto& t_b1 = b.get<scn::camera_component>(b1).color_targets[0];
	BOOST_TEST((t_a1 != t_a2));
	BOOST_TEST((t_a1 != t_b1));
	BOOST_TEST(redirector.tracked_count() == 3u);
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
	reg.emplace<scn::camera_component>(drop);
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

BOOST_AUTO_TEST_SUITE_END()
