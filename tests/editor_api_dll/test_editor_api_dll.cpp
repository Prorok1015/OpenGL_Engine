#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/editor/edt_registration.h>
#include <memory>

// What a game_editor module does with the editor API: build a panel of its own on panel_base and register it. The
// registration is kept here and dropped before the module is unloaded, as the API requires.

namespace {

class probe_panel final : public edt::panel_base
{
public:
	probe_panel() : panel_base("probe.panel", "Probe") {}

protected:
	void on_render() override {}
};

edt::registration g_panel;

}

#if defined(_WIN32)
	#define PROBE_EXPORT extern "C" __declspec(dllexport)
#else
	#define PROBE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

PROBE_EXPORT void probe_register(edt::editor_context* context)
{
	edt::panel_desc desc;
	desc.id = "probe.panel";
	desc.title = "Probe";
	desc.open_by_default = false;
	desc.show_in_view_menu = false;
	desc.panel = std::make_shared<probe_panel>();
	g_panel = context->panels().add(std::move(desc));
}

PROBE_EXPORT void probe_unregister()
{
	g_panel = edt::registration{};
}
