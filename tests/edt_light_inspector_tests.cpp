#include <boost/test/unit_test.hpp>
#include "edt_cr_internal.h"
#include "scn_directional_light_desc.h"
#include "test_scene_env.hpp"

namespace json = boost::json;

// #81: what the light inspector writes must be loadable by directional_light_desc.
BOOST_FIXTURE_TEST_SUITE(EdtLightInspectorTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(ReadVec4Override_FallsBackToDefaultWhenMissingOrMalformed)
{
	const glm::vec4 def{ 0.1f, 0.2f, 0.3f, 1.f };
	json::object o = json::parse(R"({"three":[1,2,3],"text":"x","nan":[1,"a",3,4],"ok":[1,2,3,0]})").as_object();
	BOOST_TEST((edt::read_vec4_override(o, "missing", def) == def));
	BOOST_TEST((edt::read_vec4_override(o, "three", def) == def));
	BOOST_TEST((edt::read_vec4_override(o, "text", def) == def));
	BOOST_TEST((edt::read_vec4_override(o, "nan", def) == def));
	BOOST_TEST((edt::read_vec4_override(o, "ok", def) == glm::vec4{ 1, 2, 3, 0 }));
}

BOOST_AUTO_TEST_CASE(WriteVec4Override_KeepsWAndIsAcceptedByTheLightDesc)
{
	const scn::directional_light_desc def;
	json::object ov;
	// what the inspector does after dragging the direction: x,y,z edited, w (0 for a direction) preserved
	glm::vec4 dir = edt::read_vec4_override(ov, "direction", def.direction);
	dir.x = 0.5f;
	edt::write_vec4_override(ov, "direction", dir);
	BOOST_TEST(ov.at("direction").as_array().size() == 4u);

	scn::directional_light_desc l;
	l.deserialize(*descs, ov);
	BOOST_TEST((l.direction == glm::vec4{ 0.5f, def.direction.y, def.direction.z, 0.f }));
	BOOST_TEST((l.diffuse == def.diffuse)); // untouched field keeps its default
}

BOOST_AUTO_TEST_SUITE_END()
