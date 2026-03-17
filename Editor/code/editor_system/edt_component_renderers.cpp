#include "edt_component_renderers.h"
#include "edt_component_ui_registry.h"
#include "edt_component_renderers/edt_cr_internal.h"

void edt::register_desc_component_renderers(edt::component_ui_registry& registry)
{
	edt::edt_cr_camera_register(registry);
	edt::edt_cr_light_register(registry);
	edt::edt_cr_skybox_register(registry);
	edt::edt_cr_skin_register(registry);
}
