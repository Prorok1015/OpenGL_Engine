#include <snk/render/rnd_shader_desc.h>

rnd::shader_config rnd::shader_desc::config() const
{
	shader_config out;
	out.cdata.program = shader_config::shader_program_data::build();
	out.cdata.program.set_vertex_shader(vertex);
	out.cdata.program.set_fragment_shader(fragment);
	out.cdata.program.set_geometry_shader(geometry);
	out.cdata.defines = defines;
	return out;
}
