#include <boost/test/unit_test.hpp>
#include "shader/rnd_scene_shader_desc.h"

BOOST_AUTO_TEST_SUITE(RndShaderConfigTests)

// --- shader_program_data ---

BOOST_AUTO_TEST_CASE(Build_Creates3NullSlots) {
	auto prog = rnd::shader_config::shader_program_data::build();
	BOOST_TEST(prog.size() == 3);
}

BOOST_AUTO_TEST_CASE(SetVertexShader_StoredCorrectly) {
	auto prog = rnd::shader_config::shader_program_data::build();
	auto tag = res::tag::make("shaders/basic.vert");
	prog.set_vertex_shader(tag);
	BOOST_CHECK(prog.get_vertex_shader() == tag);
}

BOOST_AUTO_TEST_CASE(SetFragmentShader_StoredCorrectly) {
	auto prog = rnd::shader_config::shader_program_data::build();
	auto tag = res::tag::make("shaders/basic.frag");
	prog.set_fragment_shader(tag);
	BOOST_CHECK(prog.get_fragment_shader() == tag);
}

BOOST_AUTO_TEST_CASE(SetGeometryShader_StoredCorrectly) {
	auto prog = rnd::shader_config::shader_program_data::build();
	auto tag = res::tag::make("shaders/wireframe.geom");
	prog.set_geometry_shader(tag);
	BOOST_CHECK(prog.get_geometry_shader() == tag);
}

BOOST_AUTO_TEST_CASE(GetShader_WhenNotSet_ReturnsNull) {
	auto prog = rnd::shader_config::shader_program_data::build();
	BOOST_TEST(!prog.get_vertex_shader().is_valid());
	BOOST_TEST(!prog.get_fragment_shader().is_valid());
	BOOST_TEST(!prog.get_geometry_shader().is_valid());
}

// --- shader_config hashing ---

BOOST_AUTO_TEST_CASE(Hash_SameConfig_SameHash) {
	rnd::shader_config a, b;
	a.cdata.program = rnd::shader_config::shader_program_data::build();
	a.cdata.program.set_vertex_shader(res::tag::make("s.vert"));
	b.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.program.set_vertex_shader(res::tag::make("s.vert"));
	BOOST_TEST(a.get_hash() == b.get_hash());
}

BOOST_AUTO_TEST_CASE(Hash_DifferentShaders_DifferentHash) {
	rnd::shader_config a, b;
	a.cdata.program = rnd::shader_config::shader_program_data::build();
	a.cdata.program.set_vertex_shader(res::tag::make("a.vert"));
	b.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.program.set_vertex_shader(res::tag::make("b.vert"));
	BOOST_TEST(a.get_hash() != b.get_hash());
}

BOOST_AUTO_TEST_CASE(Equality_SameConfigs) {
	rnd::shader_config a, b;
	a.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.program = rnd::shader_config::shader_program_data::build();
	BOOST_CHECK(a == b);
}

BOOST_AUTO_TEST_CASE(Equality_DifferentDefines) {
	rnd::shader_config a, b;
	a.cdata.program = rnd::shader_config::shader_program_data::build();
	a.cdata.defines.push_back("HAS_NORMALS");
	b.cdata.program = rnd::shader_config::shader_program_data::build();
	BOOST_CHECK(!(a == b));
}

BOOST_AUTO_TEST_CASE(Hash_DefinesAffectHash) {
	rnd::shader_config a, b;
	a.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.defines.push_back("SKINNED");
	BOOST_TEST(a.get_hash() != b.get_hash());
}

BOOST_AUTO_TEST_CASE(Hash_ConstantsAffectHash) {
	rnd::shader_config a, b;
	a.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.program = rnd::shader_config::shader_program_data::build();
	b.cdata.constants["color"] = "red";
	BOOST_TEST(a.get_hash() != b.get_hash());
}

BOOST_AUTO_TEST_SUITE_END()
