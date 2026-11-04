#include <boost/test/unit_test.hpp>
#include "edt_forge_account_panel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

// What ImGui keeps of a token typed into a field (#458 PR-4): the wipe must reach the active state, its revert copy, the
// deactivated copy and the undo stack, all inside the ImGui context.
namespace {

const std::string canary = "ghp_CANARYCANARYCANARY0123456789";

bool contains(const char* data, std::size_t size, std::string_view needle)
{
	if (data == nullptr || size < needle.size()) return false;
	return std::string_view(data, size).find(needle) != std::string_view::npos;
}

bool vector_has(const ImVector<char>& vector, std::string_view needle)
{
	return contains(vector.Data, static_cast<std::size_t>(vector.Capacity), needle);
}

// The stb undo stack and the other buffers, as bytes.
template <class state>
auto* edit_of(state& input)
{
	if constexpr (std::is_pointer_v<std::remove_reference_t<decltype(input.Stb)>>) return input.Stb;
	else return &input.Stb;
}

bool anything_left(ImGuiContext& g)
{
	if (vector_has(g.InputTextState.TextA, canary) || vector_has(g.InputTextState.TextToRevertTo, canary)) return true;
	if (vector_has(g.InputTextDeactivatedState.TextA, canary)) return true;
	if (auto* edit = edit_of(g.InputTextState)) {
		return contains(reinterpret_cast<const char*>(&edit->undostate), sizeof(edit->undostate), "CANARY");
	}
	return false;
}

struct imgui_ctx
{
	imgui_ctx()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~imgui_ctx() { ImGui::DestroyContext(ctx); }

	ImGuiContext* ctx = nullptr;
};

void fill(ImVector<char>& vector, const std::string& text)
{
	vector.resize(static_cast<int>(text.size()) + 1);
	std::memcpy(vector.Data, text.c_str(), text.size() + 1);
}

}

BOOST_AUTO_TEST_SUITE(EdtForgeAccountPanel)

BOOST_AUTO_TEST_CASE(WipeImguiTokenState_ClearsTheCopiesImguiHolds_FilledDirectly)
{
	imgui_ctx imgui;
	ImGuiContext& g = *imgui.ctx;
	const ImGuiID field = 0x1234u;
	g.InputTextState.ID = field;
	fill(g.InputTextState.TextA, canary);
	fill(g.InputTextState.TextToRevertTo, canary);
	g.InputTextDeactivatedState.ID = field;
	fill(g.InputTextDeactivatedState.TextA, canary);
	if (auto* edit = edit_of(g.InputTextState)) {
		std::memcpy(edit->undostate.undo_char, canary.data(), canary.size());
	}
	BOOST_REQUIRE(anything_left(g));

	edt::wipe_imgui_token_state(field);

	BOOST_TEST(!anything_left(g));
	BOOST_TEST((g.InputTextState.ID == 0u));
	BOOST_TEST((g.InputTextDeactivatedState.ID == 0u));
	BOOST_TEST(g.InputTextState.TextA.Size == 0);
	BOOST_TEST(g.InputTextDeactivatedState.TextA.Size == 0);
}

BOOST_AUTO_TEST_CASE(WipeImguiTokenState_LeavesAnotherFieldAlone)
{
	imgui_ctx imgui;
	ImGuiContext& g = *imgui.ctx;
	g.InputTextState.ID = 0x1u;
	fill(g.InputTextState.TextA, canary);

	edt::wipe_imgui_token_state(0x2u);

	BOOST_TEST((g.InputTextState.ID == 0x1u));
	BOOST_TEST(vector_has(g.InputTextState.TextA, canary));
	edt::wipe_imgui_token_state(0u);
	BOOST_TEST(vector_has(g.InputTextState.TextA, canary));
}

BOOST_AUTO_TEST_CASE(WipeImguiTokenState_WithoutAContext_DoesNothing)
{
	ImGuiContext* previous = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(nullptr);
	edt::wipe_imgui_token_state(0x1u);
	ImGui::SetCurrentContext(previous);
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(WipeImguiTokenState_AfterTypingInARealField_LeavesNoCanary)
{
	imgui_ctx imgui;
	ImGuiContext& g = *imgui.ctx;
	ImGuiIO& io = ImGui::GetIO();
	std::array<char, 128> buffer{};
	ImGuiID id = 0;
	const auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(400.f, 200.f));
		if (ImGui::Begin("token")) {
			id = ImGui::GetID("##token");
			if (g.InputTextState.ID != id) ImGui::SetKeyboardFocusHere();
			ImGui::InputText("##token", buffer.data(), buffer.size(),
				ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_EnterReturnsTrue);
		}
		ImGui::End();
		ImGui::EndFrame();
	};
	for (int i = 0; i < 4 && g.InputTextState.ID != id; ++i) frame();
	BOOST_REQUIRE((g.InputTextState.ID == id));

	io.AddInputCharactersUTF8(canary.c_str());
	frame();
	BOOST_REQUIRE(std::string(buffer.data()) == canary);

	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, false);
	frame();
	BOOST_TEST(!std::string(buffer.data()).empty());

	edt::wipe_buffer(buffer.data(), buffer.size());
	edt::wipe_imgui_token_state(id);

	BOOST_TEST(!anything_left(g));
	BOOST_TEST(!contains(buffer.data(), buffer.size(), "CANARY"));
}

BOOST_AUTO_TEST_SUITE_END()
