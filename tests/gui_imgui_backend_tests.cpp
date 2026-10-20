#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include <snk/driver_validation/rnd_validation_driver.h>
#include <snk/driver/rnd_texture_owner.h>
#include <snk/gui/gui_backend_interface.h>

#include <memory>

// gui::imgui_backend_interface::to_imgui: the panels hand ImGui::Image a texture_handle and the backend turns it
// into the id its renderer draws with. The renderer half is not under test here (no GL context), only the
// handle to id step, over the null driver and over the validation decorator that wraps it.

namespace {

using namespace rnd::driver;
namespace vl = rnd::driver::validation;

class stub_backend final : public gui::imgui_backend_interface
{
public:
	using imgui_backend_interface::imgui_backend_interface;

	void init() override {}
	void new_frame() override {}
	void render() override {}
	void shutdown() override {}
};

texture_header small_texture()
{
	texture_header header;
	header.data.extent.width = 4;
	header.data.extent.height = 4;
	return header;
}

}

BOOST_AUTO_TEST_SUITE(GuiImguiBackendTests)

BOOST_AUTO_TEST_CASE(ALiveTextureGetsItsOwnNonZeroId)
{
	null::driver drv;
	stub_backend backend(drv);
	auto first = make_texture(drv, small_texture());
	auto second = make_texture(drv, small_texture());

	BOOST_TEST(backend.to_imgui(first.handle()) != 0u);
	BOOST_TEST(backend.to_imgui(second.handle()) != 0u);
	BOOST_TEST(backend.to_imgui(first.handle()) != backend.to_imgui(second.handle()));
	// The id is the name the driver gives the texture, asked again it is the same.
	BOOST_TEST(backend.to_imgui(first.handle()) == drv.native_texture_name(first.handle()));
	BOOST_TEST(backend.to_imgui(first.handle()) == backend.to_imgui(first.handle()));
}

BOOST_AUTO_TEST_CASE(ANullStaleOrForeignHandleGivesZero)
{
	null::driver drv;
	null::driver other;
	stub_backend backend(drv);
	auto texture = make_texture(drv, small_texture());
	auto foreign = make_texture(other, small_texture());
	const texture_handle stale = texture.handle();

	BOOST_TEST(backend.to_imgui(texture_handle{}) == 0u);
	BOOST_TEST(backend.to_imgui(foreign.handle()) == 0u);
	BOOST_REQUIRE(backend.to_imgui(stale) != 0u);

	texture.reset();
	BOOST_TEST(backend.to_imgui(stale) == 0u);
}

BOOST_AUTO_TEST_CASE(ARecreatedTextureGetsAnotherIdAndTheOldHandleStaysZero)
{
	null::driver drv;
	stub_backend backend(drv);
	auto first = make_texture(drv, small_texture());
	const texture_handle old_handle = first.handle();
	const ImTextureID old_id = backend.to_imgui(old_handle);

	first.reset();
	auto again = make_texture(drv, small_texture());
	BOOST_TEST(backend.to_imgui(old_handle) == 0u);
	BOOST_TEST(backend.to_imgui(again.handle()) != 0u);
	BOOST_TEST(backend.to_imgui(again.handle()) != old_id);
}

BOOST_AUTO_TEST_CASE(TheValidationDecoratorGivesTheNameOfTheWrappedTexture)
{
	auto wrapped = std::make_unique<null::driver>();
	null::driver& inner = *wrapped;
	vl::driver drv(std::move(wrapped));
	stub_backend backend(drv);
	// Take a slot of the wrapped driver's so the two drivers' handles differ.
	auto occupant = make_texture(inner, small_texture());
	auto texture = make_texture(drv, small_texture());
	const texture_handle inner_handle = drv.inner_handle(texture.handle());
	BOOST_REQUIRE((inner_handle != texture.handle()));

	BOOST_TEST(backend.to_imgui(texture.handle()) != 0u);
	BOOST_TEST(backend.to_imgui(texture.handle()) == inner.native_texture_name(inner_handle));
	BOOST_TEST(backend.to_imgui(texture.handle()) != inner.native_texture_name(occupant.handle()));
	// The wrapped driver's own handle is not a texture of the decorator.
	BOOST_TEST(backend.to_imgui(inner_handle) == 0u);
	BOOST_TEST(drv.state().total() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
