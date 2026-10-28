#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_glm_json_convert.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <sstream>

namespace json = boost::json;

namespace {

json::object ser(const desc::desc_base& d)
{
	json::object o;
	d.serialize(o);
	return o;
}

// serialize -> fresh instance -> deserialize -> serialize must be a fixed point.
template<class T>
T reload(desc::desc_system& ds, const T& in)
{
	T out;
	out.deserialize(ds, ser(in));
	return out;
}

template<class T>
void check_stable(desc::desc_system& ds, const T& in)
{
	auto first = ser(in);
	T again = reload(ds, in);
	auto second = ser(again);
	BOOST_TEST(json::value(first) == json::value(second));
}

struct roundtrip_fixture : test_util::scene_env
{
	json::object parse(const char* text) { return json::parse(text).as_object(); }
};

}

BOOST_FIXTURE_TEST_SUITE(ScnDescRoundTripTests, roundtrip_fixture)

// ---------------------------------------------------------------- glm <-> json

BOOST_AUTO_TEST_CASE(GlmJson_VectorsRoundTrip)
{
	BOOST_TEST((json::value_to<glm::vec2>(json::value_from(glm::vec2{ 1.5f, -2 })) == glm::vec2{ 1.5f, -2 }));
	BOOST_TEST((json::value_to<glm::vec3>(json::value_from(glm::vec3{ 1, 2, 3 })) == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST((json::value_to<glm::vec4>(json::value_from(glm::vec4{ 1, 2, 3, 4 })) == glm::vec4{ 1, 2, 3, 4 }));
}

BOOST_AUTO_TEST_CASE(GlmJson_Mat4RoundTripsColumnMajor)
{
	glm::mat4 m(1.0f);
	m[3] = glm::vec4(7, 8, 9, 1);
	m[1][2] = 0.25f;
	auto jv = json::value_from(m);
	BOOST_TEST(jv.as_array().size() == 4u);
	BOOST_TEST((json::value_to<glm::vec4>(jv.at(3)) == glm::vec4(7, 8, 9, 1))); // 4th array entry is column 3
	BOOST_TEST((json::value_to<glm::mat4>(jv) == m));
}

BOOST_AUTO_TEST_CASE(GlmJson_QuatIsStoredWFirst)
{
	glm::quat q(0.5f, 0.1f, 0.2f, 0.3f); // w, x, y, z
	auto jv = json::value_from(q);
	BOOST_TEST(jv.at(0).to_number<float>() == 0.5f);
	BOOST_TEST(jv.at(1).to_number<float>() == 0.1f);
	auto back = json::value_to<glm::quat>(jv);
	BOOST_TEST(back.w == q.w);
	BOOST_TEST(back.x == q.x);
	BOOST_TEST(back.y == q.y);
	BOOST_TEST(back.z == q.z);
}

BOOST_AUTO_TEST_CASE(GlmJson_WrongArityOrTypeYieldsZeroValue_CurrentBehavior)
{
	// Pins current behavior: malformed vectors silently become all-zero (e.g. a zero SCALE).
	BOOST_TEST((json::value_to<glm::vec3>(json::parse("[1,2]")) == glm::vec3{ 0 }));
	BOOST_TEST((json::value_to<glm::vec3>(json::parse("\"x\"")) == glm::vec3{ 0 }));
	BOOST_TEST((json::value_to<glm::vec2>(json::parse("[1,2,3]")) == glm::vec2{ 0 }));
	BOOST_TEST((json::value_to<glm::vec4>(json::parse("{}")) == glm::vec4{ 0 }));
	BOOST_TEST((json::value_to<glm::mat4>(json::parse("[1,2]")) == glm::mat4{ 0 }));
}

// ---------------------------------------------------------------- individual descs

BOOST_AUTO_TEST_CASE(DirectionalLight_RoundTripAndDefaults)
{
	scn::directional_light_desc def;
	BOOST_TEST((def.direction == glm::vec4{ -0.2f, -1.0f, -0.3f, 0.0f }));

	auto d = descs->create_instance("directional_light_desc", res::tag{});
	d->deserialize(*descs, parse(R"({"direction":[0,-1,0,0],"diffuse":[1,0,0,1],"ambient":[0,0,1,1],"specular":[0.5,0.5,0.5,1]})"));
	auto& l = static_cast<scn::directional_light_desc&>(*d);
	BOOST_TEST((l.direction == glm::vec4{ 0, -1, 0, 0 }));
	BOOST_TEST((l.diffuse == glm::vec4{ 1, 0, 0, 1 }));
	BOOST_TEST((l.ambient == glm::vec4{ 0, 0, 1, 1 }));
	check_stable(*descs, l);
	BOOST_TEST((reload(*descs, l).specular == glm::vec4{ 0.5f, 0.5f, 0.5f, 1 }));
}

BOOST_AUTO_TEST_CASE(DirectionalLight_PartialJsonKeepsOtherFields)
{
	scn::directional_light_desc l;
	l.deserialize(*descs, parse(R"({"diffuse":[0.1,0.2,0.3,1]})"));
	BOOST_TEST((l.diffuse == glm::vec4{ 0.1f, 0.2f, 0.3f, 1 }));
	BOOST_TEST((l.ambient == glm::vec4{ 0.2f, 0.2f, 0.2f, 1 })); // untouched default
}

// #81: a 3-element colour/direction array (what the inspector used to write) must not zero the light.
BOOST_AUTO_TEST_CASE(DirectionalLight_MalformedVectorsKeepDefaultsInsteadOfZero)
{
	const scn::directional_light_desc def;
	scn::directional_light_desc l;
	l.deserialize(*descs, parse(R"({"direction":[0,-1,0],"diffuse":[1,0.5],"ambient":"x","specular":[1,"a",3,1]})"));
	BOOST_TEST((l.direction == def.direction));
	BOOST_TEST((l.diffuse == def.diffuse));
	BOOST_TEST((l.ambient == def.ambient));
	BOOST_TEST((l.specular == def.specular));

	l.deserialize(*descs, parse(R"({"diffuse":[1,0.5,0.25,1],"direction":[0,-1]})"));
	BOOST_TEST((l.diffuse == glm::vec4{ 1, 0.5f, 0.25f, 1 }));
	BOOST_TEST((l.direction == def.direction));
}

// #131: the bytes of the light as the hand-written serializer wrote them (compact json::serialize, captured from CI on that code).
// The generic serializer must keep them.
BOOST_AUTO_TEST_CASE(DirectionalLight_SerializedBytesMatchReference)
{
	BOOST_TEST(json::serialize(ser(scn::directional_light_desc{}))
		== R"({"direction":[-2.0000000298023224E-1,-1E0,-3.0000001192092896E-1,0E0],)"
		R"("diffuse":[5E-1,5E-1,5E-1,1E0],"ambient":[2.0000000298023224E-1,2.0000000298023224E-1,2.0000000298023224E-1,1E0],)"
		R"("specular":[1E0,1E0,1E0,1E0]})");

	// The light of projects/sandbox/res/levels/test_level.desc.
	scn::directional_light_desc l;
	l.deserialize(*descs, parse(R"({"__type":"directional_light_desc","__version":1,)"
		R"("direction":[0.3860273063182831,-0.8251869082450867,-0.41237056255340576,0.0],)"
		R"("diffuse":[1.0,1.0,1.0,1.0],"ambient":[0.0,0.0,0.0,1.0],"specular":[0.0,0.0,0.0,1.0]})"));
	BOOST_TEST(json::serialize(ser(l))
		== R"({"direction":[3.860273063182831E-1,-8.251869082450867E-1,-4.1237056255340576E-1,0E0],)"
		R"("diffuse":[1E0,1E0,1E0,1E0],"ambient":[0E0,0E0,0E0,1E0],"specular":[0E0,0E0,0E0,1E0]})");

	auto created = descs->create_instance("directional_light_desc", res::tag{});
	BOOST_TEST(json::serialize(ser(*created)) == json::serialize(ser(scn::directional_light_desc{})));
}

// #131: what reaches the .desc file goes through the desc_system writer; its text for the vectors is what test_level.desc holds.
BOOST_AUTO_TEST_CASE(DirectionalLight_OnDiskTextMatchesReference)
{
	const auto on_disk = [this](const json::value& v) {
		std::ostringstream ss;
		descs->write_pretty_json(ss, v);
		return ss.str();
	};

	const json::object defaults = ser(scn::directional_light_desc{});
	BOOST_TEST(defaults.size() == 4u);
	BOOST_TEST(on_disk(defaults.at("direction")) == "[-0.20000000298023224, -1.0, -0.30000001192092896, 0.0]\n");
	BOOST_TEST(on_disk(defaults.at("diffuse")) == "[0.5, 0.5, 0.5, 1.0]\n");
	BOOST_TEST(on_disk(defaults.at("ambient")) == "[0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 1.0]\n");
	BOOST_TEST(on_disk(defaults.at("specular")) == "[1.0, 1.0, 1.0, 1.0]\n");
	BOOST_TEST((std::vector<std::string>{ defaults.begin()->key(), (defaults.begin() + 1)->key(), (defaults.begin() + 2)->key(), (defaults.begin() + 3)->key() }
		== std::vector<std::string>{ "direction", "diffuse", "ambient", "specular" }));

	scn::directional_light_desc l;
	l.deserialize(*descs, parse(R"({"direction":[0.3860273063182831,-0.8251869082450867,-0.41237056255340576,0.0]})"));
	BOOST_TEST(on_disk(ser(l).at("direction")) == "[0.3860273063182831, -0.8251869082450867, -0.41237056255340576, 0.0]\n");
}

// #131: every form the hand-written deserialize accepted.
BOOST_AUTO_TEST_CASE(DirectionalLight_AcceptsLegacyForms)
{
	const scn::directional_light_desc def;

	scn::directional_light_desc empty;
	empty.deserialize(*descs, parse("{}"));
	BOOST_TEST((empty.direction == def.direction && empty.diffuse == def.diffuse && empty.ambient == def.ambient && empty.specular == def.specular));

	scn::directional_light_desc l;
	l.deserialize(*descs, parse(R"({"__type":"directional_light_desc","__version":1,"unknown":[1,2],"direction":[0,-1,0,0],"diffuse":[2,0,1,1],"specular":[1e0,0.25,-3,0]})"));
	BOOST_TEST((l.direction == glm::vec4{ 0, -1, 0, 0 }));
	BOOST_TEST((l.diffuse == glm::vec4{ 2, 0, 1, 1 }));
	BOOST_TEST((l.ambient == def.ambient));
	BOOST_TEST((l.specular == glm::vec4{ 1, 0.25f, -3, 0 }));

	// null, an object, a longer array and a non-numeric component keep the current value of just that field.
	scn::directional_light_desc bad;
	bad.diffuse = glm::vec4{ 0.1f, 0.2f, 0.3f, 1 };
	bad.deserialize(*descs, parse(R"({"direction":null,"diffuse":{"r":1},"ambient":[1,1,1,1,1],"specular":[true,0,0,1]})"));
	BOOST_TEST((bad.direction == def.direction));
	BOOST_TEST((bad.diffuse == glm::vec4{ 0.1f, 0.2f, 0.3f, 1 }));
	BOOST_TEST((bad.ambient == def.ambient));
	BOOST_TEST((bad.specular == def.specular));
}

BOOST_AUTO_TEST_CASE(BoneDesc_RoundTripKeepsMatrixAndIndex)
{
	scn::bone_desc b;
	b.index = 5;
	b.offset_matrix = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
	auto r = reload(*descs, b);
	BOOST_TEST(r.index == 5);
	BOOST_TEST((r.offset_matrix == b.offset_matrix));
	BOOST_TEST(scn::bone_desc{}.index == -1);
}

BOOST_AUTO_TEST_CASE(SkinningDesc_RoundTripKeepsBoneCountAndTag)
{
	scn::skinning_desc s;
	s.bone_count = 42;
	s.skinning_tag = res::tag{ "memory://models/hero.skin" };
	auto r = reload(*descs, s);
	BOOST_TEST(r.bone_count == 42);
	BOOST_TEST(r.skinning_tag == s.skinning_tag);

	scn::skinning_desc empty;
	BOOST_TEST(!ser(empty).contains("skinning_tag")); // invalid tag is not written
}

BOOST_AUTO_TEST_CASE(SkinWeightsDesc_RoundTripPreservesFullUint32Range)
{
	scn::skin_weights_desc w;
	w.registration_tag = res::tag{ "memory://models/hero.desc" };
	w.weights = { { 1, 2, 0x3F800000u, 0x3F000000u }, {}, { 0xFFFFFFFFu } };
	auto r = reload(*descs, w);
	BOOST_TEST(r.registration_tag == w.registration_tag);
	BOOST_REQUIRE(r.weights.size() == 3u);
	BOOST_TEST((r.weights[0] == std::vector<uint32_t>{ 1, 2, 0x3F800000u, 0x3F000000u }));
	BOOST_TEST(r.weights[1].empty());
	BOOST_TEST(r.weights[2][0] == 0xFFFFFFFFu);
}

BOOST_AUTO_TEST_CASE(AnimationControllerDesc_DefaultsAndRoundTrip)
{
	scn::animation_controller_desc def;
	BOOST_TEST(def.speed == 1.0f);
	BOOST_TEST(def.autoplay);
	BOOST_TEST(def.loop);
	BOOST_TEST(!ser(def).contains("default_animation"));

	scn::animation_controller_desc c;
	c.default_animation = "Walk";
	c.speed = 2.5f;
	c.autoplay = false;
	c.loop = false;
	auto r = reload(*descs, c);
	BOOST_TEST(r.default_animation == "Walk");
	BOOST_TEST(r.speed == 2.5f);
	BOOST_TEST(!r.autoplay);
	BOOST_TEST(!r.loop);
}

BOOST_AUTO_TEST_CASE(AnimationClipDesc_RoundTripPreservesKeysOfAllThreeKinds)
{
	scn::animation_clip_desc clip;
	clip.name = "Walk";
	clip.duration = 12.f;
	clip.ticks_per_second = 30.f;
	scn::animation_node n;
	n.pos_keys.push_back({ glm::vec3{ 0, 0, 0 }, 0.f });
	n.pos_keys.push_back({ glm::vec3{ 1, 2, 3 }, 6.f });
	n.rotate_keys.push_back({ glm::quat{ 1, 0, 0, 0 }, 0.f });
	n.rotate_keys.push_back({ glm::quat{ 0.5f, 0.5f, 0.5f, 0.5f }, 12.f });
	n.scale_keys.push_back({ glm::vec3{ 2, 2, 2 }, 3.f });
	clip.channels["Hip"] = n;
	clip.channels["Empty"] = {};

	auto r = reload(*descs, clip);
	BOOST_TEST(r.name == "Walk");
	BOOST_TEST(r.duration == 12.f);
	BOOST_TEST(r.ticks_per_second == 30.f);
	BOOST_REQUIRE(r.channels.size() == 2u);
	const auto& hip = r.channels.at("Hip");
	BOOST_REQUIRE(hip.pos_keys.size() == 2u);
	BOOST_TEST((hip.pos_keys[1].value == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST(hip.pos_keys[1].time == 6.f);
	BOOST_REQUIRE(hip.rotate_keys.size() == 2u);
	BOOST_TEST(hip.rotate_keys[1].value.w == 0.5f);
	BOOST_TEST(hip.rotate_keys[1].time == 12.f);
	BOOST_REQUIRE(hip.scale_keys.size() == 1u);
	BOOST_TEST((hip.scale_keys[0].value == glm::vec3{ 2, 2, 2 }));
	BOOST_TEST(r.channels.at("Empty").pos_keys.empty());
	check_stable(*descs, clip);
}

BOOST_AUTO_TEST_CASE(AnimationClipDesc_DefaultsWhenFieldsMissing)
{
	scn::animation_clip_desc c;
	c.deserialize(*descs, parse(R"({"channels":{"bad":5,"ok":{}}})"));
	BOOST_TEST(c.ticks_per_second == 25.f);
	BOOST_TEST(c.duration == 0.f);
	BOOST_TEST(c.channels.contains("ok"));
	BOOST_TEST(!c.channels.contains("bad")); // non-object channels are skipped
}

BOOST_AUTO_TEST_CASE(AnimationCollectionDesc_InlineClipsLoadAndRoundTrip)
{
	auto tag = put("anims.desc", R"({"__type":"animation_collection_desc","clips":[
		{"__type":"animation_clip_desc","name":"Idle","duration":4.0,"ticks_per_second":24.0,"channels":{}},
		{"__type":"animation_clip_desc","name":"Run","duration":8.0,"ticks_per_second":30.0,"channels":{}}]})");
	auto coll = load<scn::animation_collection_desc>(tag);
	BOOST_REQUIRE(coll);
	BOOST_REQUIRE(coll->clips.size() == 2u);
	BOOST_TEST(coll->clips[0].get_sync()->name == "Idle");
	BOOST_TEST(coll->clips[1].get_sync()->duration == 8.0f);

	json::object out = ser(*coll);
	BOOST_TEST(out.at("clips").as_array().size() == 2u);
	BOOST_TEST(out.at("clips").at(1).at("name").as_string() == "Run");
	BOOST_TEST(out.at("clips").at(1).at("__type").as_string() == "animation_clip_desc");

	// serialize() omits the top-level "__type" (see DescAdapter_SerializeKeepsTypeForEveryDescType), so add it like the editor does
	out["__type"] = "animation_collection_desc";
	auto again = load<scn::animation_collection_desc>(put("anims2.desc", json::serialize(out).data()));
	BOOST_REQUIRE(again);
	BOOST_REQUIRE(again->clips.size() == 2u);
	BOOST_TEST(again->clips[0].get_sync()->name == "Idle");
}

BOOST_AUTO_TEST_CASE(AnimationCollectionComponent_FindsReadyClipByName)
{
	auto coll = load<scn::animation_collection_desc>(put("anims.desc", R"({"__type":"animation_collection_desc","clips":[
		{"__type":"animation_clip_desc","name":"Idle","duration":4.0},
		{"__type":"animation_clip_desc","name":"Run","duration":8.0}]})"));
	scn::animation_collection_component comp;
	comp.clips = coll->clips;
	BOOST_REQUIRE(comp.find("Run"));
	BOOST_TEST(comp.find("Run")->duration == 8.0f);
	BOOST_TEST(comp.find("Jump") == nullptr);
}

BOOST_AUTO_TEST_CASE(AnimationCollection_AssembleAutoCreatesControllerFromFirstClip)
{
	auto coll = load<scn::animation_collection_desc>(put("anims.desc", R"({"__type":"animation_collection_desc","clips":[
		{"__type":"animation_clip_desc","name":"Idle","duration":4.0,"ticks_per_second":24.0}]})"));
	entt::registry reg;
	auto e = reg.create();
	scn::assemble_animation_collection(reg, e, *coll, "anims");
	BOOST_REQUIRE(reg.all_of<scn::animation_controller_component>(e));
	const auto& ctrl = reg.get<scn::animation_controller_component>(e);
	BOOST_TEST(ctrl.current_animation == "Idle");
	BOOST_TEST(ctrl.duration == 4.0f);
	BOOST_TEST(ctrl.ticks_per_second == 24.0f);
	BOOST_TEST(ctrl.playing);

	// an existing controller is not overwritten
	auto e2 = reg.create();
	reg.emplace<scn::animation_controller_component>(e2).current_animation = "Custom";
	scn::assemble_animation_collection(reg, e2, *coll, "anims");
	BOOST_TEST(reg.get<scn::animation_controller_component>(e2).current_animation == "Custom");
}

BOOST_AUTO_TEST_CASE(MarkerDescs_SerializeToEmptyObjects)
{
	BOOST_TEST(ser(scn::object_desc{}).empty());
	BOOST_TEST(ser(scn::scene_anchor_desc{}).empty());
	BOOST_TEST(ser(scn::animated_node_desc{}).empty());
}

BOOST_AUTO_TEST_CASE(MeshNodeDesc_RoundTripIndexRanges)
{
	scn::mesh_node_desc m;
	m.vx_begin = 4;
	m.vx_end = 400;
	m.ind_begin = 8;
	m.ind_end = 1200;
	auto r = reload(*descs, m);
	BOOST_TEST(r.vx_begin == 4u);
	BOOST_TEST(r.vx_end == 400u);
	BOOST_TEST(r.ind_begin == 8u);
	BOOST_TEST(r.ind_end == 1200u);
	BOOST_TEST(!r.material.is_valid());
	BOOST_TEST(!ser(m).contains("material"));
	BOOST_TEST(!ser(m).contains("geometry"));
}

BOOST_AUTO_TEST_CASE(CameraDesc_DeserializeReadsAllFieldsIncludingDegreesToRadians)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"fov":60.0,"near_distance":0.5,"far_distance":500.0,
		"transform":{"rotation":[0,90,0],"position":[1,2,3]},
		"color_targets":["memory://rt/a.tex","memory://rt/b.tex"],
		"depth_target":"memory://rt/d.tex","tp_accum_target":"memory://rt/acc.tex","tp_reveal_target":"memory://rt/rev.tex"})"));
	BOOST_TEST(c.fov() == 60.0f);
	BOOST_TEST(c.near_distance() == 0.5f);
	BOOST_TEST(c.far_distance() == 500.0f);
	BOOST_TEST(c.rotation().y == glm::radians(90.0f), boost::test_tools::tolerance(1e-5f));
	BOOST_TEST((c.position() == glm::vec3{ 1, 2, 3 }));
	BOOST_REQUIRE(c.color_targets().size() == 2u);
	BOOST_TEST(c.color_targets()[1] == res::tag{ "memory://rt/b.tex" });
	BOOST_TEST(c.depth_target() == res::tag{ "memory://rt/d.tex" });
	BOOST_TEST(c.tp_accum_target() == res::tag{ "memory://rt/acc.tex" });
	BOOST_TEST(c.tp_reveal_target() == res::tag{ "memory://rt/rev.tex" });
}

BOOST_AUTO_TEST_CASE(CameraDesc_LegacySingleTextureBecomesFirstColorTarget)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"texture":"memory://rt/old.tex"})"));
	BOOST_REQUIRE(c.color_targets().size() == 1u);
	BOOST_TEST(c.color_targets()[0] == res::tag{ "memory://rt/old.tex" });
}

BOOST_AUTO_TEST_CASE(CameraDesc_ProtocolLessRenderTargetsNormalizeToMemory)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"color_targets":["__rt_color","memory://__rt_keep","rt/sub"],
		"depth_target":"__rt_depth","tp_accum_target":"__rt_acc","tp_reveal_target":"__rt_rev"})"));
	BOOST_REQUIRE(c.color_targets().size() == 3u);
	BOOST_TEST(c.color_targets()[0] == res::tag(res::tag::memory, "__rt_color"));
	BOOST_TEST(c.color_targets()[1] == res::tag{ "memory://__rt_keep" });
	BOOST_TEST(c.color_targets()[2] == res::tag(res::tag::memory, "rt/sub"));
	BOOST_TEST(c.depth_target() == res::tag(res::tag::memory, "__rt_depth"));
	BOOST_TEST(c.tp_accum_target() == res::tag(res::tag::memory, "__rt_acc"));
	BOOST_TEST(c.tp_reveal_target() == res::tag(res::tag::memory, "__rt_rev"));
	BOOST_TEST(c.depth_target().protocol() == "memory");
	BOOST_TEST(c.depth_target().name() == "__rt_depth");

	auto r = reload(*descs, c);
	BOOST_TEST(r.depth_target() == c.depth_target());
	BOOST_TEST(r.color_targets()[0] == c.color_targets()[0]);
	BOOST_TEST(ser(c).at("depth_target").as_string() == "memory://__rt_depth");
}

BOOST_AUTO_TEST_CASE(CameraDesc_LegacyProtocolLessTextureNormalizes)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"texture":"__rt_old"})"));
	BOOST_REQUIRE(c.color_targets().size() == 1u);
	BOOST_TEST(c.color_targets()[0] == res::tag(res::tag::memory, "__rt_old"));
}

BOOST_AUTO_TEST_CASE(CameraDesc_SerializeRoundTripsRenderSettings)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"fov":45.0,"near_distance":0.1,"far_distance":99.0,
		"color_targets":["memory://rt/a.tex"],"depth_target":"memory://rt/d.tex"})"));
	auto r = reload(*descs, c);
	BOOST_TEST(r.fov() == 45.0f);
	BOOST_TEST(r.far_distance() == 99.0f);
	BOOST_REQUIRE(r.color_targets().size() == 1u);
	BOOST_TEST(r.depth_target() == c.depth_target());
	BOOST_TEST(!ser(c).contains("tp_accum_target")); // invalid tags are omitted
}

BOOST_AUTO_TEST_CASE(CameraDesc_SerializeKeepsTransform)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"transform":{"rotation":[0,90,0],"position":[1,2,3]}})"));
	auto r = reload(*descs, c);
	BOOST_TEST((r.position() == glm::vec3{ 1, 2, 3 }));
}

BOOST_AUTO_TEST_CASE(CameraDesc_OverrideReplacesInheritedColorTargets)
{
	scn::camera_desc parent;
	parent.deserialize(*descs, parse(R"({"color_targets":["memory://rt/a.tex"]})"));
	scn::camera_desc child;
	parent.copy_to(child);
	child.deserialize(*descs, parse(R"({"color_targets":["memory://rt/b.tex"]})"));
	BOOST_REQUIRE(child.color_targets().size() >= 1u);
	BOOST_TEST(child.color_targets().size() == 1u);
}

BOOST_AUTO_TEST_CASE(CameraDesc_AcceptsIntegerNumbers)
{
	scn::camera_desc c;
	BOOST_CHECK_NO_THROW(c.deserialize(*descs, parse(R"({"fov":60})")));
}

BOOST_AUTO_TEST_CASE(CameraDesc_RenderLayersRoundTripAndTheDefaultsAreNotWritten)
{
	scn::camera_desc plain;
	plain.deserialize(*descs, parse(R"({"fov":60.0})"));
	BOOST_TEST(plain.layer() == 0);
	BOOST_TEST((plain.clear() == scn::camera_clear::all));
	BOOST_TEST((plain.projection() == scn::camera_projection::perspective));
	const auto written = ser(plain);
	// cameras saved before the fields existed stay byte-identical
	BOOST_TEST(!written.contains("layer"));
	BOOST_TEST(!written.contains("clear"));
	BOOST_TEST(!written.contains("projection"));
	BOOST_TEST(!written.contains("ortho_size"));

	scn::camera_desc overlay;
	overlay.deserialize(*descs, parse(R"({"layer":1,"clear":"depth","projection":"ortho","ortho_size":4})"));
	BOOST_TEST(overlay.layer() == 1);
	BOOST_TEST((overlay.clear() == scn::camera_clear::depth));
	BOOST_TEST((overlay.projection() == scn::camera_projection::ortho));
	BOOST_TEST(overlay.ortho_size() == 4.0f);
	const auto out = ser(overlay);
	BOOST_TEST(out.at("layer").as_int64() == 1);
	BOOST_TEST(out.at("clear").as_string() == "depth");
	BOOST_TEST(out.at("projection").as_string() == "ortho");
	const auto back = reload(*descs, overlay);
	BOOST_TEST(back.layer() == 1);
	BOOST_TEST((back.clear() == scn::camera_clear::depth));
	BOOST_TEST((back.projection() == scn::camera_projection::ortho));
	BOOST_TEST(back.ortho_size() == 4.0f);
	check_stable(*descs, overlay);

	scn::camera_desc none;
	none.deserialize(*descs, parse(R"({"clear":"none"})"));
	BOOST_TEST(ser(none).at("clear").as_string() == "none");
}

BOOST_AUTO_TEST_CASE(CameraDesc_NonIntegerLayerIsIgnoredAndOutOfRangeIsClamped)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"layer":2})"));
	c.deserialize(*descs, parse(R"({"layer":1.5})"));
	BOOST_TEST(c.layer() == 2);
	c.deserialize(*descs, parse(R"({"layer":100})"));
	BOOST_TEST(c.layer() == 16);
	c.deserialize(*descs, parse(R"({"layer":-100})"));
	BOOST_TEST(c.layer() == -16);
	c.deserialize(*descs, parse(R"({"layer":18446744073709551615})"));
	BOOST_TEST(c.layer() == 16);
}

BOOST_AUTO_TEST_CASE(CameraDesc_UnknownLayerWordsKeepTheCurrentValues)
{
	scn::camera_desc c;
	c.deserialize(*descs, parse(R"({"layer":2,"clear":"depth","projection":"ortho"})"));
	c.deserialize(*descs, parse(R"({"clear":"sideways","projection":7,"layer":"top"})"));
	BOOST_TEST(c.layer() == 2);
	BOOST_TEST((c.clear() == scn::camera_clear::depth));
	BOOST_TEST((c.projection() == scn::camera_projection::ortho));
}

// #131-b: the bytes of the camera as the hand-written serializer wrote them (compact json::serialize, captured from CI on that
// code). The generic serializer must keep them.
BOOST_AUTO_TEST_CASE(CameraDesc_SerializedBytesMatchReference)
{
	BOOST_TEST(json::serialize(ser(scn::camera_desc{})) == R"({"fov":9E1,"near_distance":9.999999747378752E-5,"far_distance":1E3,)"
		R"("transform":{"rotation":[0E0,0E0,0E0],"position":[0E0,0E0,0E0]}})");

	// Every field set, the keys of the W4 render settings written because they are not the defaults.
	scn::camera_desc full;
	full.deserialize(*descs, parse(R"({"__type":"camera_desc","__version":1,"fov":37.3,"near_distance":0.05,"far_distance":250.5,
		"transform":{"rotation":[10.5,90,-45.25],"position":[1,2.5,-3]},
		"color_targets":["memory://rt/a.tex","memory://rt/b.tex"],
		"depth_target":"memory://rt/d.tex","tp_accum_target":"memory://rt/acc.tex","tp_reveal_target":"memory://rt/rev.tex",
		"layer":-2,"clear":"none","projection":"ortho","ortho_size":4.5})"));
	BOOST_TEST(json::serialize(ser(full)) == R"({"fov":3.729999923706055E1,"near_distance":5.000000074505806E-2,"far_distance":2.505E2,)"
		R"("transform":{"rotation":[1.05E1,9E1,-4.525E1],"position":[1E0,2.5E0,-3E0]},)"
		R"("color_targets":["memory://rt/a.tex","memory://rt/b.tex"],"depth_target":"memory://rt/d.tex",)"
		R"("tp_accum_target":"memory://rt/acc.tex","tp_reveal_target":"memory://rt/rev.tex",)"
		R"("layer":-2,"clear":"none","projection":"ortho","ortho_size":4.5E0})");

	// A camera of the shipped files: no targets, no W4 keys.
	scn::camera_desc plain;
	plain.deserialize(*descs, parse(R"({"fov":60.0,"transform":{"rotation":[0,0,0],"position":[0,1,5]}})"));
	BOOST_TEST(json::serialize(ser(plain)) == R"({"fov":6E1,"near_distance":9.999999747378752E-5,"far_distance":1E3,)"
		R"("transform":{"rotation":[0E0,0E0,0E0],"position":[0E0,1E0,5E0]}})");

	// Only some of the W4 keys: "clear":"depth" alone, and layer alone.
	scn::camera_desc overlay;
	overlay.deserialize(*descs, parse(R"({"color_targets":["memory://rt/a.tex"],"layer":1,"clear":"depth"})"));
	BOOST_TEST(json::serialize(ser(overlay)) == R"({"fov":9E1,"near_distance":9.999999747378752E-5,"far_distance":1E3,)"
		R"("transform":{"rotation":[0E0,0E0,0E0],"position":[0E0,0E0,0E0]},)"
		R"("color_targets":["memory://rt/a.tex"],"layer":1,"clear":"depth"})");

	auto created = descs->create_instance("camera_desc", res::tag{});
	BOOST_TEST(json::serialize(ser(*created)) == json::serialize(ser(scn::camera_desc{})));
}

// #131-b: what reaches the .desc file goes through the desc_system writer.
BOOST_AUTO_TEST_CASE(CameraDesc_OnDiskTextMatchesReference)
{
	const auto on_disk = [this](const json::value& v) {
		std::ostringstream ss;
		descs->write_pretty_json(ss, v);
		return ss.str();
	};

	scn::camera_desc full;
	full.deserialize(*descs, parse(R"({"fov":37.3,"near_distance":0.05,"far_distance":250.5,
		"transform":{"rotation":[10.5,90,-45.25],"position":[1,2.5,-3]},
		"color_targets":["memory://rt/a.tex","memory://rt/b.tex"],"depth_target":"memory://rt/d.tex",
		"layer":-2,"clear":"none","projection":"ortho","ortho_size":4.5})"));
	BOOST_TEST(on_disk(ser(full)) == "{\n"
		"    \"fov\" : 37.29999923706055,\n"
		"    \"near_distance\" : 0.05000000074505806,\n"
		"    \"far_distance\" : 250.5,\n"
		"    \"transform\" : {\n"
		"        \"rotation\" : [10.5, 90.0, -45.25],\n"
		"        \"position\" : [1.0, 2.5, -3.0]\n"
		"    },\n"
		"    \"color_targets\" : [\"memory://rt/a.tex\", \"memory://rt/b.tex\"],\n"
		"    \"depth_target\" : \"memory://rt/d.tex\",\n"
		"    \"layer\" : -2,\n"
		"    \"clear\" : \"none\",\n"
		"    \"projection\" : \"ortho\",\n"
		"    \"ortho_size\" : 4.5\n"
		"}\n");
	BOOST_TEST(on_disk(ser(scn::camera_desc{})) == "{\n"
		"    \"fov\" : 90.0,\n"
		"    \"near_distance\" : 9.999999747378752e-05,\n"
		"    \"far_distance\" : 1000.0,\n"
		"    \"transform\" : {\n"
		"        \"rotation\" : [0.0, 0.0, 0.0],\n"
		"        \"position\" : [0.0, 0.0, 0.0]\n"
		"    }\n"
		"}\n");

	// the key order is the order of the members, "transform" sits after far_distance
	const json::object written = ser(full);
	std::vector<std::string> keys;
	for (const auto& member : written)
		keys.emplace_back(member.key());
	BOOST_TEST((keys == std::vector<std::string>{ "fov", "near_distance", "far_distance", "transform", "color_targets", "depth_target",
		"layer", "clear", "projection", "ortho_size" }));
	BOOST_TEST(json::serialize(written.at("transform")) == R"({"rotation":[1.05E1,9E1,-4.525E1],"position":[1E0,2.5E0,-3E0]})");
}

// #131-b: every form the hand-written deserialize accepted or ignored.
BOOST_AUTO_TEST_CASE(CameraDesc_AcceptsLegacyAndMalformedForms)
{
	const scn::camera_desc def;

	// no transform at all, an unknown key, the legacy "texture" next to a missing "color_targets"
	scn::camera_desc legacy;
	legacy.deserialize(*descs, parse(R"({"__type":"camera_desc","unknown":[1,2],"fov":50,"texture":"memory://rt/old.tex"})"));
	BOOST_TEST(legacy.fov() == 50.0f);
	BOOST_TEST((legacy.position() == def.position()));
	BOOST_REQUIRE(legacy.color_targets().size() == 1u);
	BOOST_TEST(legacy.color_targets()[0] == res::tag{ "memory://rt/old.tex" });
	const json::object legacy_out = ser(legacy);
	BOOST_TEST(!legacy_out.contains("texture"));
	BOOST_TEST(json::serialize(legacy_out.at("color_targets")) == R"(["memory://rt/old.tex"])");

	// "color_targets" wins over the legacy key
	scn::camera_desc both;
	both.deserialize(*descs, parse(R"({"texture":"memory://rt/old.tex","color_targets":["memory://rt/new.tex"]})"));
	BOOST_REQUIRE(both.color_targets().size() == 1u);
	BOOST_TEST(both.color_targets()[0] == res::tag{ "memory://rt/new.tex" });

	// only one half of the transform
	scn::camera_desc half;
	half.deserialize(*descs, parse(R"({"transform":{"position":[4,5,6]}})"));
	BOOST_TEST((half.position() == glm::vec3{ 4, 5, 6 }));
	BOOST_TEST((half.rotation() == def.rotation()));

	// invalid tags are not written, empty target lists neither
	scn::camera_desc tags;
	tags.deserialize(*descs, parse(R"({"depth_target":"memory://rt/d.tex"})"));
	const json::object tags_out = ser(tags);
	BOOST_TEST(tags_out.contains("depth_target"));
	BOOST_TEST(!tags_out.contains("color_targets"));
	BOOST_TEST(!tags_out.contains("tp_accum_target"));
	BOOST_TEST(!tags_out.contains("tp_reveal_target"));
	BOOST_TEST(!ser(def).contains("depth_target"));

	// a layer that is not an integer (even an integral double) keeps the current one, a clamp applies to integers only
	scn::camera_desc layered;
	layered.deserialize(*descs, parse(R"({"layer":5})"));
	for (const char* bad : { R"({"layer":3.0})", R"({"layer":1.5})", R"({"layer":"7"})", R"({"layer":null})", R"({"layer":true})", R"({"layer":[1]})" }) {
		layered.deserialize(*descs, parse(bad));
		BOOST_TEST_INFO(bad);
		BOOST_TEST(layered.layer() == 5);
	}
	layered.deserialize(*descs, parse(R"({"layer":2147483648})"));
	BOOST_TEST(layered.layer() == 16);
	layered.deserialize(*descs, parse(R"({"layer":-9223372036854775807})"));
	BOOST_TEST(layered.layer() == -16);

	// an unknown word, or a value that is not a string, keeps the current "clear" and "projection"
	scn::camera_desc words;
	words.deserialize(*descs, parse(R"({"clear":"none","projection":"ortho"})"));
	for (const char* bad : { R"({"clear":"sideways","projection":"sideways"})", R"({"clear":1,"projection":1})", R"({"clear":null,"projection":false})", R"({"clear":["all"],"projection":["perspective"]})" }) {
		words.deserialize(*descs, parse(bad));
		BOOST_TEST_INFO(bad);
		BOOST_TEST((words.clear() == scn::camera_clear::none));
		BOOST_TEST_INFO(bad);
		BOOST_TEST((words.projection() == scn::camera_projection::ortho));
	}

	// numbers may be integers; a value that is not a number keeps the current one
	scn::camera_desc numbers;
	numbers.deserialize(*descs, parse(R"({"fov":60,"near_distance":1,"far_distance":200,"ortho_size":3})"));
	BOOST_TEST(numbers.fov() == 60.0f);
	BOOST_TEST(numbers.near_distance() == 1.0f);
	BOOST_TEST(numbers.far_distance() == 200.0f);
	BOOST_TEST(numbers.ortho_size() == 3.0f);

	// a transform that is not an object is ignored
	scn::camera_desc bad_transform;
	bad_transform.deserialize(*descs, parse(R"({"transform":5,"fov":45})"));
	BOOST_TEST(bad_transform.fov() == 45.0f);
	BOOST_TEST((bad_transform.position() == def.position()));

	// more targets than the camera has slots keep the current ones
	scn::camera_desc many;
	many.deserialize(*descs, parse(R"({"color_targets":["memory://a"]})"));
	many.deserialize(*descs, parse(R"({"color_targets":["memory://1","memory://2","memory://3","memory://4","memory://5"]})"));
	BOOST_REQUIRE(many.color_targets().size() == 1u);
	BOOST_TEST(many.color_targets()[0] == res::tag{ "memory://a" });

	// the ortho_size written when it differs from the default, however slightly
	scn::camera_desc ortho;
	ortho.deserialize(*descs, parse(R"({"ortho_size":10.000001})"));
	BOOST_TEST(ser(ortho).contains("ortho_size") == (ortho.ortho_size() != def.ortho_size()));
}

BOOST_AUTO_TEST_CASE(CameraDesc_AssembleCopiesTheRenderLayersToTheComponent)
{
	scn::camera_desc desc;
	desc.deserialize(*descs, parse(R"({"layer":3,"clear":"none","projection":"ortho","ortho_size":8.0})"));
	entt::registry reg;
	const auto ent = reg.create();
	scn::assemble_camera(reg, ent, desc, "cam");
	const auto& cam = reg.get<scn::camera_component>(ent);
	BOOST_TEST(cam.layer == 3);
	BOOST_TEST((cam.clear == scn::camera_clear::none));
	BOOST_TEST((cam.projection == scn::camera_projection::ortho));
	BOOST_TEST(cam.ortho_size == 8.0f);
}

BOOST_AUTO_TEST_CASE(DescCopyTo_CopiesDataButNotIdentity)
{
	auto tag = put("cam.desc", R"({"__type":"camera_desc","fov":33.0})");
	auto cam = load<scn::camera_desc>(tag);
	scn::camera_desc other;
	cam->copy_to(other);
	BOOST_TEST(other.fov() == 33.0f);
	BOOST_TEST(!other.get_tag().is_valid()); // resource identity stays per instance
	BOOST_TEST(other.get_type() == cam->get_type());
}

// ---------------------------------------------------------------- desc adapter (what pin_resource / save use)

BOOST_AUTO_TEST_CASE(DescAdapter_SerializeThenDeserializeRoundTripsMaterialAndWorld)
{
	core::desc::desc_adapter_t adapter(*rs, *descs);
	// these two types write their own "__type", so the adapter output is directly reloadable
	for (const char* type : { "material_desc", "world_desc" }) {
		auto inst = descs->create_instance(type, tag_of("saved.desc"));
		auto bytes = adapter.serialize(tag_of("saved.desc"), inst);
		std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		BOOST_TEST_CONTEXT(type) {
			BOOST_TEST(json::parse(text).as_object().at("__type").as_string() == type);
			auto back = adapter.deserialize(tag_of("saved.desc"), bytes);
			BOOST_TEST(static_cast<desc::desc_base&>(*std::static_pointer_cast<desc::desc_base>(back)).get_type() == type);
		}
	}
}

BOOST_AUTO_TEST_CASE(DescAdapter_SerializeKeepsTypeForEveryDescType)
{
	core::desc::desc_adapter_t adapter(*rs, *descs);
	std::vector<std::string> missing;
	for (const char* type : { "prefab_desc", "world_desc", "level_desc", "anchor_desc", "camera_desc",
			"directional_light_desc", "skybox_desc", "mesh_node_desc", "object_desc", "bone_desc",
			"skinning_desc", "skin_weights_desc", "animation_controller_desc", "animated_node_desc",
			"animation_clip_desc", "animation_collection_desc", "material_desc" }) {
		auto inst = descs->create_instance(type, tag_of("saved.desc"));
		auto bytes = adapter.serialize(tag_of("saved.desc"), inst);
		auto parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		if (!parsed.as_object().contains("__type")) missing.emplace_back(type);
	}
	std::string joined;
	for (auto& m : missing) joined += m + " ";
	BOOST_TEST(missing.empty(), "types whose adapter output has no __type: " << joined);
}

// ---------------------------------------------------------------- desc_system itself

BOOST_AUTO_TEST_CASE(DescSystem_CreateInstanceSetsTagAndType)
{
	auto inst = descs->create_instance("stat_desc", tag_of("x.desc"));
	BOOST_REQUIRE(inst);
	BOOST_TEST(inst->get_tag() == tag_of("x.desc"));
	BOOST_TEST(inst->get_type() == "stat_desc");
	BOOST_TEST(!descs->create_instance("not_registered", res::tag{}));
}

BOOST_AUTO_TEST_CASE(DescSystem_UnregisterRemovesFactoryAndComponentName)
{
	descs->register_component_desc<test_util::stat_desc>("stat_component_desc");
	const auto& names = descs->get_component_type_names();
	BOOST_TEST(std::ranges::count(names, "stat_component_desc") == 1);
	BOOST_TEST(std::ranges::count(names, "object_desc") == 0); // register_desc is not user-addable
	BOOST_TEST(std::ranges::count(names, "camera_desc") == 1);

	descs->unregister_desc("stat_component_desc");
	BOOST_TEST(std::ranges::count(descs->get_component_type_names(), "stat_component_desc") == 0);
	BOOST_TEST(!descs->create_instance("stat_component_desc", res::tag{}));
	BOOST_CHECK_NO_THROW(descs->unregister_desc("never_registered"));
}

BOOST_AUTO_TEST_CASE(DescSystem_MemTagIsDeterministicAndContentAddressed)
{
	auto owner = descs->create_instance("stat_desc", tag_of("owner.desc"));
	json::value a = json::parse(R"({"__type":"stat_desc","hp":1})");
	json::value b = json::parse(R"({"__type":"stat_desc","hp":2})");

	auto t1 = descs->make_mem_tag_deterministic(*owner, "field", a);
	auto t2 = descs->make_mem_tag_deterministic(*owner, "field", a);
	auto t3 = descs->make_mem_tag_deterministic(*owner, "field", b);
	auto t4 = descs->make_mem_tag_deterministic(*owner, "other", a);

	BOOST_TEST(t1 == t2);
	BOOST_TEST(!(t1 == t3));
	BOOST_TEST(!(t1 == t4));
	BOOST_TEST(t1.protocol() == "memory");
	BOOST_TEST(t1.extension() == "desc");
	BOOST_TEST(t1.string().find("overrides/owner/field_") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(DescSystem_GetFieldDescStoresInlineObjectOnceAndLoadsIt)
{
	auto owner = descs->create_instance("stat_desc", tag_of("owner.desc"));
	json::value inline_obj = json::parse(R"({"__type":"stat_desc","hp":12,"label":"inline"})");

	auto h1 = descs->get_field_desc<desc::desc_base>(*owner, inline_obj);
	auto h2 = descs->get_field_desc<desc::desc_base>(*owner, inline_obj);
	BOOST_REQUIRE(h1.get_sync());
	BOOST_TEST(static_cast<test_util::stat_desc&>(*h1.get_sync()).hp == 12);
	BOOST_TEST(h1.get_tag() == h2.get_tag());
	BOOST_TEST(ram->files.size() == 1u); // second call found it in the store and did not rewrite
}

BOOST_AUTO_TEST_CASE(DescSystem_GetFieldDescWithStringLoadsThatTag)
{
	auto tag = put("ref.desc", R"({"__type":"stat_desc","hp":3})");
	auto owner = descs->create_instance("stat_desc", res::tag{});
	auto h = descs->get_field_desc<desc::desc_base>(*owner, json::value(tag.string()));
	BOOST_TEST(h.get_tag() == tag);
	BOOST_TEST(static_cast<test_util::stat_desc&>(*h.get_sync()).hp == 3);
}

BOOST_AUTO_TEST_CASE(DescSystem_PrettyJsonParsesBackToTheSameValue)
{
	json::value v = json::parse(R"({"s":"quote \" and \\ and \n","n":[1,2.5,-3,true,false,null],
		"o":{"nested":{"deep":[{"a":1},{}],"empty_arr":[]},"e":{}}})");
	std::stringstream ss;
	descs->write_pretty_json(ss, v);
	auto text = ss.str();
	BOOST_TEST(text.find('\n') != std::string::npos);
	BOOST_TEST(text.back() == '\n');
	BOOST_TEST(json::value(json::parse(text)) == v);

	auto bytes = descs->serialize_to_bytes(v);
	std::string as_text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	BOOST_TEST(json::value(json::parse(as_text)) == v);
}

// ---------------------------------------------------------------- regression: #82 (save dropped the transform of a reference-only node)

BOOST_AUTO_TEST_CASE(WorldDesc_NodeReferencingAssetKeepsTransformOnSave)
{
	put("robot.desc", R"({"__type":"stat_desc","hp":5})");
	auto text = std::string(R"({"__type":"world_desc","__version":3,"name":"w","systems":[],"children":[
		{"__id":"00000000000000b1","name":"scene","transform":{"rotation":[-90,0,0],"position":[0,-1,-6],"scale":[0.1,0.1,0.1]},
			"components":{"scene":")") + tag_of("robot.desc").string() + R"("}}]})";

	scn::world_desc w;
	w.deserialize(*descs, parse_migrated(text, "world_desc"));
	auto out = ser(w);

	const auto& scene = out.at("children").as_array().at(0);
	BOOST_REQUIRE_MESSAGE(scene.is_object(), "a node is always an object");
	BOOST_TEST(scene.at("name").as_string() == "scene");
	const auto& tr = scene.as_object().at("transform").as_object();
	BOOST_TEST((json::value_to<glm::vec3>(tr.at("position")) == glm::vec3{ 0, -1, -6 }));
	BOOST_TEST((json::value_to<glm::vec3>(tr.at("rotation")) == glm::vec3{ -90, 0, 0 }));
	check_stable(*descs, w);
}

BOOST_AUTO_TEST_CASE(WorldDesc_IdentityReferenceNodeIsWrittenAsANodeWithAReferenceComponent)
{
	put("robot.desc", R"({"__type":"stat_desc","hp":5})");
	auto text = std::string(R"({"__type":"world_desc","__version":3,"name":"w","systems":[],"children":[
		{"__id":"00000000000000b1","name":"scene","components":{"scene":")") + tag_of("robot.desc").string() + R"("}}]})";

	scn::world_desc w;
	w.deserialize(*descs, parse_migrated(text, "world_desc"));
	const auto out = ser(w);
	const auto& scene = out.at("children").as_array().at(0).as_object();
	BOOST_TEST(scene.at("name").as_string() == "scene");
	BOOST_TEST(scene.at("components").at("scene").as_string() == tag_of("robot.desc").string());
	BOOST_TEST(!scene.contains("transform"));
	check_stable(*descs, w);
}

// ---------------------------------------------------------------- input_layer (#271)

BOOST_AUTO_TEST_CASE(WorldDesc_InputLayerRoundTripsAndTheDefaultIsNotWritten)
{
	scn::world_desc ui;
	ui.deserialize(*descs, parse(R"({"__type":"world_desc","name":"ui","systems":[],"input_layer":1,"children":[]})"));
	BOOST_TEST(ui.get_input_layer() == 1);
	auto out = ser(ui);
	BOOST_TEST(out.at("input_layer").as_int64() == 1);
	scn::world_desc back;
	back.deserialize(*descs, out);
	BOOST_TEST(back.get_input_layer() == 1);

	scn::world_desc plain;
	plain.deserialize(*descs, parse(R"({"__type":"world_desc","name":"w","systems":[],"children":[]})"));
	BOOST_TEST(plain.get_input_layer() == 0);
	BOOST_TEST(!ser(plain).contains("input_layer")); // levels saved before the field stay byte-identical
}

// ---------------------------------------------------------------- material_desc (#131-c, #321)

namespace {

// What a material file reads and writes back: the bytes of the compact json (the pretty writer is the same for every desc).
struct material_io
{
	desc::desc_system& descs;

	scn::material_desc read(const char* text) const
	{
		scn::material_desc m;
		m.deserialize(descs, json::parse(text).as_object());
		return m;
	}

	std::string resave(const char* text) const { return json::serialize(ser(read(text))); }
};

}

// The two materials of core/res, header keys as they are on disk. The generic writer must give the bytes the hand-written
// one gave (desc_resave --check holds the files themselves).
BOOST_AUTO_TEST_CASE(MaterialDesc_ShippedFilesResaveToTheSameBytes)
{
	const material_io io{ *descs };
	BOOST_TEST(io.resave(R"({"__type":"material_desc","__version":1,"__guid":"f90d3c03-865c-4193-aa09-cb2f98315634",
		"defines":[],"samplers":[],"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag",
		"shader_geometry":null,"queue":"opaque","uniforms":{"albedo":[1.0,1.0,1.0,1.0],"emissive":[0.0,0.0,0.0,0.0]},
		"render_mode":"TRIANGLE"})")
		== R"({"defines":[],"samplers":[],"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag",)"
		R"("shader_geometry":null,"queue":"opaque","uniforms":{"albedo":[1E0,1E0,1E0,1E0],"emissive":[0E0,0E0,0E0,0E0]},)"
		R"("render_mode":"TRIANGLE"})");

	BOOST_TEST(io.resave(R"({"__type":"material_desc","defines":[],"samplers":["res://skybox/sky_texture_cubemap.desc"],
		"shader_vertex":"res://shaders/sky.vert","shader_fragment":"res://shaders/sky.frag","shader_geometry":null,"queue":"opaque",
		"uniforms":{"albedo":[1.0,1.0,1.0,1.0],"emissive":[0.0,0.0,0.0,0.0]},"render_mode":"TRIANGLE"})")
		== R"({"defines":[],"samplers":["res://skybox/sky_texture_cubemap.desc"],"shader_vertex":"res://shaders/sky.vert",)"
		R"("shader_fragment":"res://shaders/sky.frag","shader_geometry":null,"queue":"opaque",)"
		R"("uniforms":{"albedo":[1E0,1E0,1E0,1E0],"emissive":[0E0,0E0,0E0,0E0]},"render_mode":"TRIANGLE"})");
}

BOOST_AUTO_TEST_CASE(MaterialDesc_OnDiskTextMatchesTheShippedFile)
{
	std::ostringstream ss;
	descs->write_pretty_json(ss, ser(material_io{ *descs }.read(R"({"defines":[],"samplers":[],
		"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag","shader_geometry":null,
		"queue":"opaque","uniforms":{"albedo":[1.0,1.0,1.0,1.0],"emissive":[0.0,0.0,0.0,0.0]},"render_mode":"TRIANGLE"})")));
	BOOST_TEST(ss.str() == "{\n"
		"    \"defines\" : [],\n"
		"    \"samplers\" : [],\n"
		"    \"shader_vertex\" : \"res://shaders/scene.vert\",\n"
		"    \"shader_fragment\" : \"res://shaders/scene.frag\",\n"
		"    \"shader_geometry\" : null,\n"
		"    \"queue\" : \"opaque\",\n"
		"    \"uniforms\" : {\n"
		"        \"albedo\" : [1.0, 1.0, 1.0, 1.0],\n"
		"        \"emissive\" : [0.0, 0.0, 0.0, 0.0]\n"
		"    },\n"
		"    \"render_mode\" : \"TRIANGLE\"\n"
		"}\n");
}

// What the model importer writes: a hole in the sampler slots is a null, the tags of the shaders come from the parent (so
// they are empty here and written as null), shininess is a number of the file.
BOOST_AUTO_TEST_CASE(MaterialDesc_ImporterFormKeepsTheSlotHoleAndNowKeepsShininess)
{
	const material_io io{ *descs };
	const char* imported = R"({"name":"mat","defines":["LIGHTS_ENABLED","USE_SPECULAR_MAP"],"samplers":[null,"res://spec.png"],
		"uniforms":{"albedo":[0.5,0.25,0.5,1],"emissive":[0,0,0,1],"shininess":16}})";
	const scn::material_desc m = io.read(imported);
	BOOST_TEST(m.samplers_textures.size() == 2u);
	BOOST_TEST(!m.samplers_textures[0].is_valid());
	BOOST_TEST(m.samplers_textures[1].string() == "res://spec.png");
	BOOST_TEST(m.shininess == 16.0f);

	const std::string written = json::serialize(ser(m));
	BOOST_TEST(written == R"({"defines":["LIGHTS_ENABLED","USE_SPECULAR_MAP"],"samplers":[null,"res://spec.png"],)"
		R"("shader_vertex":null,"shader_fragment":null,"shader_geometry":null,"queue":"opaque",)"
		R"("uniforms":{"albedo":[5E-1,2.5E-1,5E-1,1E0],"emissive":[0E0,0E0,0E0,1E0],"shininess":1.6E1},"render_mode":"TRIANGLE"})");
	// and a second round trip is a fixed point
	BOOST_TEST(io.resave(written.c_str()) == written);
}

// #321: shininess was read from uniforms but never written, so a changed value was lost on the next save.
BOOST_AUTO_TEST_CASE(MaterialDesc_ShininessIsWrittenWhenItIsNotTheDefault)
{
	const material_io io{ *descs };

	scn::material_desc changed;
	changed.shininess = 64.0f;
	const json::object written = ser(changed);
	BOOST_REQUIRE(written.at("uniforms").as_object().if_contains("shininess"));
	BOOST_TEST(written.at("uniforms").at("shininess").to_number<double>() == 64.0);
	BOOST_TEST(reload(*descs, changed).shininess == 64.0f);
	// after the colours, so the keys of the files without it keep their places
	std::vector<std::string> keys;
	for (const auto& member : written.at("uniforms").as_object())
		keys.emplace_back(member.key());
	BOOST_TEST((keys == std::vector<std::string>{ "albedo", "emissive", "shininess" }));

	// zero is not the default either
	scn::material_desc none;
	none.shininess = 0.0f;
	BOOST_TEST(ser(none).at("uniforms").as_object().if_contains("shininess") != nullptr);
	BOOST_TEST(reload(*descs, none).shininess == 0.0f);

	// the default is not written, so the files that never had it stay as they are
	BOOST_TEST(!ser(scn::material_desc{}).at("uniforms").as_object().contains("shininess"));
	BOOST_TEST(!ser(io.read(R"({"uniforms":{"shininess":32}})")).at("uniforms").as_object().contains("shininess"));
	BOOST_TEST(io.read(R"({"uniforms":{"shininess":32}})").shininess == 32.0f);
	BOOST_TEST(io.read(R"({"uniforms":{"albedo":[1,1,1,1]}})").shininess == 32.0f); // the key is optional
}

BOOST_AUTO_TEST_CASE(MaterialDesc_QueueReadsWordsAndNullAndWritesNullForNone)
{
	const material_io io{ *descs };
	const std::pair<const char*, scn::pass_queue> forms[] = {
		{ R"({"queue":"opaque"})", scn::pass_queue::OPAQUE },
		{ R"({"queue":"transparent"})", scn::pass_queue::TRANSPARENT },
		{ R"({"queue":"mix"})", scn::pass_queue::MIX },
		{ R"({"queue":null})", scn::pass_queue::NONE },
		{ R"({"queue":"bogus"})", scn::pass_queue::NONE }, // an unknown word was no queue
		{ R"({"queue":"OPAQUE"})", scn::pass_queue::NONE }, // the words are lower case
		{ R"({"queue":1})", scn::pass_queue::NONE },       // a number is not a word, even one that names an enumerator
		{ R"({"queue":true})", scn::pass_queue::NONE },
		{ R"({})", scn::pass_queue::OPAQUE },               // absent: the default stays
	};
	for (const auto& [text, expected] : forms) {
		BOOST_TEST_CONTEXT(text) BOOST_CHECK((io.read(text).queue == expected));
	}

	for (const char* word : { "opaque", "transparent", "mix" }) {
		const std::string text = std::string(R"({"queue":")") + word + R"("})";
		const json::object out = ser(io.read(text.c_str()));
		BOOST_TEST_CONTEXT(word) BOOST_TEST(out.at("queue").as_string() == word);
	}
	const json::object none = ser(io.read(R"({"queue":null})"));
	BOOST_REQUIRE(none.if_contains("queue"));
	BOOST_TEST(none.at("queue").is_null()); // an explicit null, not a missing key and not "NONE"
}

BOOST_AUTO_TEST_CASE(MaterialDesc_RenderModeUsesTheEnumeratorNames)
{
	const material_io io{ *descs };
	for (const char* name : { "TRIANGLE", "TRIANGLE_STRIP", "LINE", "LINE_STRIP", "POINT" }) {
		const std::string text = std::string(R"({"render_mode":")") + name + R"("})";
		BOOST_TEST_CONTEXT(name) BOOST_TEST(ser(io.read(text.c_str())).at("render_mode").as_string() == name);
	}
	BOOST_CHECK((io.read(R"({"render_mode":"BOGUS"})").render_mode == rnd::driver::RENDER_MODE::TRIANGLE));
	BOOST_CHECK((io.read(R"({"render_mode":"LINE_LOOP"})").render_mode == rnd::driver::RENDER_MODE::TRIANGLE));	// removed in R4
	BOOST_CHECK((io.read(R"({})").render_mode == rnd::driver::RENDER_MODE::TRIANGLE));
	BOOST_CHECK((io.read(R"({"render_mode":"POINT"})").render_mode == rnd::driver::RENDER_MODE::POINT));
}

BOOST_AUTO_TEST_CASE(MaterialDesc_EmptyTagsAreWrittenAsNullAndNullOrEmptyReadAsEmpty)
{
	const material_io io{ *descs };
	for (const char* text : { R"({})", R"({"shader_vertex":null,"shader_fragment":"","shader_geometry":null})" }) {
		const scn::material_desc m = io.read(text);
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(!m.vertex_shader.is_valid());
			BOOST_TEST(!m.fragment_shader.is_valid());
			BOOST_TEST(!m.geometry_shader.is_valid());
			const json::object out = ser(m);
			for (const char* key : { "shader_vertex", "shader_fragment", "shader_geometry" }) {
				BOOST_REQUIRE_MESSAGE(out.if_contains(key), key);
				BOOST_CHECK_MESSAGE(out.at(key).is_null(), key);
			}
		}
	}
	// null clears a tag that was set, as it did
	scn::material_desc m = io.read(R"({"shader_geometry":"res://g.geom"})");
	BOOST_TEST(m.geometry_shader.string() == "res://g.geom");
	m.deserialize(*descs, json::parse(R"({"shader_geometry":null})").as_object());
	BOOST_TEST(!m.geometry_shader.is_valid());
	// a sampler that is not a string is an empty slot
	const scn::material_desc slots = io.read(R"({"samplers":[1,"res://a.png",null]})");
	BOOST_REQUIRE_EQUAL(slots.samplers_textures.size(), 3u);
	BOOST_TEST(!slots.samplers_textures[0].is_valid());
	BOOST_TEST(slots.samplers_textures[1].string() == "res://a.png");
	BOOST_TEST(!slots.samplers_textures[2].is_valid());
}

// The members the file never had are neither read nor written, but a copy of the desc (a prefab layer) carries them.
BOOST_AUTO_TEST_CASE(MaterialDesc_AmbientAndSpecularStayOutOfTheFileButSurviveACopy)
{
	const material_io io{ *descs };
	const scn::material_desc m = io.read(R"({"uniforms":{"ambient":[0,1,0,1],"specular":[0,0,1,1]}})");
	BOOST_CHECK((m.ambient == ds::color(1.0f)));
	BOOST_CHECK((m.specular == ds::color(1.0f)));
	const json::object out = ser(m);
	BOOST_TEST(!out.at("uniforms").as_object().contains("ambient"));
	BOOST_TEST(!out.at("uniforms").as_object().contains("specular"));
	BOOST_TEST(!out.contains("ambient"));

	scn::material_desc source;
	source.ambient = ds::color(0.25f);
	source.specular = ds::color(0.5f);
	source.shininess = 8.0f;
	scn::material_desc target;
	source.copy_to(target);
	BOOST_CHECK((target.ambient == ds::color(0.25f)));
	BOOST_CHECK((target.specular == ds::color(0.5f)));
	BOOST_TEST(target.shininess == 8.0f);
}

BOOST_AUTO_TEST_CASE(MaterialDesc_ADamagedValueKeepsWhatTheMaterialHas)
{
	scn::material_desc m;
	m.albedo = ds::color(0.5f);
	m.deserialize(*descs, json::parse(R"({"uniforms":{"albedo":[1,2],"emissive":"x","shininess":"hard"},"defines":3})").as_object());
	BOOST_CHECK((m.albedo == ds::color(0.5f)));
	BOOST_CHECK((m.emissive == ds::color(0.0f)));
	BOOST_TEST(m.shininess == 32.0f);
	BOOST_TEST(m.defines.empty());
}

// The declaration of the schema: the keys (the file), the policies (what is written when), the enums.
BOOST_AUTO_TEST_CASE(MaterialDesc_SchemaDeclaresTheKeysAndThePolicies)
{
	const desc::type_schema* schema = descs->find_schema(scn::material_desc::__type);
	BOOST_REQUIRE(schema);
	using wp = desc::write_policy;
	struct expected { const char* name; const char* key; desc::field_kind kind; wp write; };
	const expected fields[] = {
		{ "defines", "/defines", desc::field_kind::array, wp::always },
		{ "samplers_textures", "/samplers", desc::field_kind::custom, wp::always },
		{ "vertex_shader", "/shader_vertex", desc::field_kind::res_tag, wp::null_if_empty },
		{ "fragment_shader", "/shader_fragment", desc::field_kind::res_tag, wp::null_if_empty },
		{ "geometry_shader", "/shader_geometry", desc::field_kind::res_tag, wp::null_if_empty },
		{ "queue", "/queue", desc::field_kind::enumeration, wp::null_if_empty },
		{ "albedo", "/uniforms/albedo", desc::field_kind::vec4, wp::always },
		{ "emissive", "/uniforms/emissive", desc::field_kind::vec4, wp::always },
		{ "shininess", "/uniforms/shininess", desc::field_kind::f32, wp::if_not_default },
		{ "render_mode", "/render_mode", desc::field_kind::enumeration, wp::always },
	};
	BOOST_REQUIRE_EQUAL(schema->fields.size(), std::size(fields));
	for (std::size_t i = 0; i < std::size(fields); ++i) {
		BOOST_TEST_CONTEXT(fields[i].name) {
			BOOST_TEST(schema->fields[i].name == fields[i].name);
			BOOST_TEST(schema->fields[i].key == fields[i].key);
			BOOST_CHECK(schema->fields[i].kind == fields[i].kind);
			BOOST_CHECK(schema->fields[i].write == fields[i].write);
		}
	}
	BOOST_TEST(schema->fields[8].default_value.to_number<double>() == 32.0);
	BOOST_REQUIRE(schema->fields[8].range);
	BOOST_TEST(schema->fields[8].range->min == 0.0);
	BOOST_TEST(schema->fields[8].range->max == 1000.0);

	const desc::enum_schema* queue = descs->schemas().find_enum("pass_queue");
	BOOST_REQUIRE(queue);
	BOOST_REQUIRE(queue->null_value);
	BOOST_TEST(*queue->null_value == static_cast<std::int64_t>(scn::pass_queue::NONE));
	BOOST_REQUIRE(queue->fallback);
	BOOST_TEST(*queue->fallback == static_cast<std::int64_t>(scn::pass_queue::NONE));
	BOOST_CHECK(queue->find("opaque") != nullptr);
	BOOST_CHECK(queue->find("OPAQUE") == nullptr);

	const desc::enum_schema* mode = descs->schemas().find_enum("render_mode");
	BOOST_REQUIRE(mode);
	BOOST_REQUIRE(mode->fallback);
	BOOST_TEST(*mode->fallback == static_cast<std::int64_t>(rnd::driver::RENDER_MODE::TRIANGLE));
	BOOST_TEST(mode->items.size() == 5u);
}

BOOST_AUTO_TEST_SUITE_END()
