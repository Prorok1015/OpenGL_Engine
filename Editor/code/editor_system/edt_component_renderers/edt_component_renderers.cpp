#include "edt_component_renderers.h"
#include "edt_component_renderers/edt_cr_internal.h"

std::vector<edt::registration> edt::register_desc_component_renderers(edt::inspector_registry& registry)
{
	std::vector<registration> registrations;
	renderer_registrar registrar{ registry, registrations };
	edt::edt_cr_camera_register(registrar);
	edt::edt_cr_light_register(registrar);
	edt::edt_cr_skybox_register(registrar);
	edt::edt_cr_skin_register(registrar);
	edt::edt_cr_animation_register(registrar);
	return registrations;
}
