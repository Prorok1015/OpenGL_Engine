#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "scn_glm_json_convert.h"
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
	auto text = std::string(R"({"__type":"world_desc","name":"w","systems":[],"children":{
		"scene":{"transform":{"rotation":[-90,0,0],"position":[0,-1,-6],"scale":[0.1,0.1,0.1]},
			"components":{"scene":")") + tag_of("robot.desc").string() + R"("}}}})";

	scn::world_desc w;
	w.deserialize(*descs, parse(text.c_str()));
	auto out = ser(w);

	const auto& scene = out.at("children").as_object().at("scene");
	BOOST_REQUIRE_MESSAGE(scene.is_object(), "node with a transform must not collapse to a bare string");
	const auto& tr = scene.as_object().at("transform").as_object();
	BOOST_TEST((json::value_to<glm::vec3>(tr.at("position")) == glm::vec3{ 0, -1, -6 }));
	BOOST_TEST((json::value_to<glm::vec3>(tr.at("rotation")) == glm::vec3{ -90, 0, 0 }));
	check_stable(*descs, w);
}

BOOST_AUTO_TEST_CASE(WorldDesc_IdentityReferenceNodeStillUsesShorthand)
{
	put("robot.desc", R"({"__type":"stat_desc","hp":5})");
	auto text = std::string(R"({"__type":"world_desc","name":"w","systems":[],"children":{"scene":")")
		+ tag_of("robot.desc").string() + R"("}})";

	scn::world_desc w;
	w.deserialize(*descs, parse(text.c_str()));
	// The one-line form cannot hold a node id, so only an id-less node collapses.
	w.get_root().children[0].node_id = 0;
	BOOST_TEST(ser(w).at("children").as_object().at("scene").is_string());
}

BOOST_AUTO_TEST_SUITE_END()
