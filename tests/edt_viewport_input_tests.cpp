#include <boost/test/unit_test.hpp>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_input_router.h>
#include "edt_input_binding.h"
#include <imgui.h>
#include <memory>
#include <optional>

// #368 (F43), rechecked with #545: dragging a gizmo handle must not also feed the camera. The image of the viewport has
// no ImGui id, so ImGui calls it hovered while the mouse is over a handle drawn above it (and on the frame the handle is
// pressed); the viewport therefore binds the image only after the handles are submitted, and not while any item is under
// or held by the mouse.

namespace {

struct viewport_frames
{
	viewport_frames()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~viewport_frames() { ImGui::DestroyContext(ctx); }
	viewport_frames(const viewport_frames&) = delete;
	viewport_frames& operator=(const viewport_frames&) = delete;

	// What viewport_panel::on_render does: the image, the gizmo handle over it, and only then the binding.
	void frame()
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(500.f, 400.f));
		if (ImGui::Begin("viewport")) {
			ImGui::Dummy(ImVec2(300.f, 200.f));
			image_min = ImGui::GetItemRectMin();
			const edt::view_item view = edt::capture_view_item();
			router.clear_bindings();

			ImGui::SetCursorScreenPos(ImVec2(image_min.x + 100.f, image_min.y + 50.f));
			ImGui::InvisibleButton("handle", ImVec2(40.f, 40.f));

			bound = edt::bind_view_input(&router, target, view, blocked);
		}
		ImGui::End();
		ImGui::EndFrame();
	}

	void settle() { for (int i = 0; i < 3; ++i) frame(); }
	void mouse_at_image(float dx, float dy) { ImGui::GetIO().AddMousePosEvent(image_min.x + dx, image_min.y + dy); }

	ImGuiContext* ctx = nullptr;
	inp::input_router router{ std::make_shared<inp::ecs_input_manager>() };
	std::optional<inp::input_target> target{ inp::input_target{ .set = "level", .context = "w0" } };
	ImVec2 image_min{ 0.f, 0.f };
	bool bound = false;
	bool blocked = false;
};

}

BOOST_AUTO_TEST_SUITE(EdtViewportInput)

BOOST_AUTO_TEST_CASE(TheImageBindsTheInputOnlyWhereNothingIsOnTopOfIt)
{
	viewport_frames ui;
	ui.frame();

	ui.mouse_at_image(20.f, 20.f);
	ui.settle();
	BOOST_TEST(ui.bound);
	BOOST_TEST(ui.router.binding_count() == 1u);

	ui.mouse_at_image(120.f, 70.f); // over the handle
	ui.settle();
	BOOST_TEST(!ui.bound);
	BOOST_TEST(ui.router.binding_count() == 0u);

	ui.mouse_at_image(250.f, 150.f); // away from it again
	ui.settle();
	BOOST_TEST(ui.bound);
}

BOOST_AUTO_TEST_CASE(AViewThatTracksItsOwnDragBindsNothing)
{
	viewport_frames ui;
	ui.frame();
	ui.mouse_at_image(20.f, 20.f);
	ui.settle();
	BOOST_TEST(ui.bound);

	ui.blocked = true;
	ui.frame();
	BOOST_TEST(!ui.bound);
	BOOST_TEST(ui.router.binding_count() == 0u);
}

BOOST_AUTO_TEST_CASE(ADraggedHandleKeepsTheInputFromTheCameraEvenWhenTheMouseLeavesIt)
{
	viewport_frames ui;
	ui.frame();

	ui.mouse_at_image(120.f, 70.f);
	ui.settle();
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	ui.frame();
	BOOST_TEST(!ui.bound);

	ui.mouse_at_image(20.f, 20.f); // the drag goes on over the image
	ui.frame();
	ui.frame();
	BOOST_TEST(!ui.bound);
	BOOST_TEST(ui.router.binding_count() == 0u);

	ImGui::GetIO().AddMouseButtonEvent(0, false);
	ui.settle();
	BOOST_TEST(ui.bound);
}

BOOST_AUTO_TEST_CASE(NothingBindsWithoutATargetOrARouter)
{
	viewport_frames ui;
	ui.frame();
	ui.mouse_at_image(20.f, 20.f);
	ui.settle();
	BOOST_TEST(ui.bound);

	ui.target.reset();
	ui.settle();
	BOOST_TEST(!ui.bound);

	ImGui::NewFrame();
	ImGui::Begin("x");
	ImGui::Dummy(ImVec2(10.f, 10.f));
	BOOST_TEST(!edt::bind_item_input(nullptr, ui.target));
	BOOST_TEST(!edt::bind_view_input(nullptr, ui.target, edt::capture_view_item()));
	ImGui::End();
	ImGui::EndFrame();
}

BOOST_AUTO_TEST_SUITE_END()
