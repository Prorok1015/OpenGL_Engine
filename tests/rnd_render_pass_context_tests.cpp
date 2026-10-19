#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include "rnd_test_render_context.hpp"
#include "rnd_composition_pass.h"
#include "rnd_frame_upload_pass.h"
#include "rnd_opaque_pass.h"
#include "rnd_skybox_pass.h"
#include "rnd_transparent_pass.h"
#include "rnd_z_prepass.h"
#include <snk/common/mem_allocator.h>
#include <memory>
#include <memory_resource>
#include <vector>

// The passes run on nothing but a render_context: a test implements it and runs them without a render_system,
// a texture, shader or geometry manager.

namespace {

using namespace rnd;

std::vector<std::unique_ptr<render_pass_interface>> scene_passes()
{
	std::vector<std::unique_ptr<render_pass_interface>> passes;
	passes.push_back(std::make_unique<frame_upload_pass>());
	passes.push_back(std::make_unique<z_prepass>());
	passes.push_back(std::make_unique<opaque_pass>());
	passes.push_back(std::make_unique<skybox_pass>());
	passes.push_back(std::make_unique<transparent_pass>());
	passes.push_back(std::make_unique<composition_pass>());
	return passes;
}

}

BOOST_AUTO_TEST_SUITE(RndRenderPassContextTests)

BOOST_AUTO_TEST_CASE(PassesDoNothingForAFrameWithoutPackets)
{
	driver::null::driver drv;
	test_util::test_render_context context;
	for (const auto& pass : scene_passes()) {
		BOOST_REQUIRE_NO_THROW(pass->execute(context, drv));
	}
	BOOST_TEST(drv.calls().render_pass_begins == 0u);
	BOOST_TEST(drv.calls().draws == 0u);

	std::pmr::vector<render_packet_t> none(ds::frame_allocator());
	context.use_packets(&none);
	for (const auto& pass : scene_passes()) {
		BOOST_REQUIRE_NO_THROW(pass->execute(context, drv));
	}
	BOOST_TEST(drv.calls().render_pass_begins == 0u);
}

BOOST_AUTO_TEST_CASE(APassTalksToTheContextForEverythingItNeeds)
{
	ds::frame_allocator_reset();
	driver::null::driver drv;
	test_util::test_render_context context;
	std::pmr::vector<render_packet_t> packets(ds::frame_allocator());
	render_packet_t packet;
	packet.camera.viewport = { 0, 0, 64, 64 };
	packets.push_back(std::move(packet));
	context.use_packets(&packets);

	// A camera on the backbuffer without a draw: the opaque pass still clears its rectangle in a pass of its own.
	opaque_pass pass;
	BOOST_REQUIRE_NO_THROW(pass.execute(context, drv));
	BOOST_TEST(drv.calls().render_pass_begins == 1u);
	BOOST_TEST(drv.calls().render_pass_ends == 1u);
	BOOST_TEST(drv.calls().draws == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
