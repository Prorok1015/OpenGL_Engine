#include <snk/editor/edt_editor_extension.h>
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/editor/edt_selection_service.h>
#include <imgui.h>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

// The project's editor code: a panel and an inspector renderer registered into the editor. It is built as its own
// library (game_editor) and never reaches the shipping executable. What is registered here is removed when the module
// unloads. See docs/roadmap/design/EDITOR_EXTENSION_API.md in the engine repository for the registries.

namespace {

class hello_panel final : public edt::panel_base
{
public:
	explicit hello_panel(const edt::selection_service& selection) : panel_base("game.hello", "Game"), m_selection(selection) {}

protected:
	void on_render() override
	{
		ImGui::TextUnformatted("This panel comes from the project's game_editor module.");
		ImGui::TextDisabled("%s", m_selection.has_selection() ? "A node is selected." : "Nothing is selected.");
	}

private:
	const edt::selection_service& m_selection;
};

bool draw_game_note(scn::prefab_desc::prefab_comp_node& comp)
{
	std::string text;
	if (const auto it = comp.overrides.find("text"); it != comp.overrides.end() && it->value().is_string()) {
		text = std::string(it->value().as_string());
	}
	char buffer[256];
	const std::size_t size = std::min(text.size(), sizeof(buffer) - 1);
	std::copy_n(text.data(), size, buffer);
	buffer[size] = '\0';
	if (!ImGui::InputText("Text", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
		return false;
	}
	comp.overrides["text"] = std::string(buffer);
	return true;
}

class project_editor_module final : public edt::editor_extension
{
public:
	std::string_view name() const override { return SNK_GAME_EDITOR_MODULE_NAME; }

	void register_editor(edt::editor_module_context& context) override
	{
		edt::panel_desc panel;
		panel.id = "game.hello";
		panel.title = "Game";
		panel.open_by_default = false;
		panel.panel = std::make_shared<hello_panel>(context.selection());
		context.add_panel(std::move(panel));

		// A desc type of this project, not a built-in one: the editor refuses a second renderer for a built-in type.
		context.add_inspector("game_note", draw_game_note);
	}
};

}

SNK_DECLARE_EDITOR_MODULE(project_editor_module)
