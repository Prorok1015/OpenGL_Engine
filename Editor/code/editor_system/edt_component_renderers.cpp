#include "edt_component_renderers.h"
#include "edt_inspector_panel.h"
#include "edt_component_renderers/edt_cr_internal.h"

void edt::register_desc_component_renderers(edt::inspector_panel& panel)
{
	edt::edt_cr_camera_register(panel);
	edt::edt_cr_light_register(panel);
	edt::edt_cr_skybox_register(panel);
	edt::edt_cr_skin_register(panel);
}
