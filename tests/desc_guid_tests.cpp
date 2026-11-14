#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/desc_guid.h>
#include <snk/scene/scn_camera_desc.h>
#include <boost/json.hpp>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;

namespace {

bool is_guid_v4(std::string_view g)
{
	if (g.size() != 36) return false;
	for (std::size_t i = 0; i < g.size(); ++i) {
		const char c = g[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') return false;
		} else if (i == 14) {
			if (c != '4') return false;
		} else if (i == 19) {
			if (c != '8' && c != '9' && c != 'a' && c != 'b') return false;
		} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> bytes(s.size());
	std::memcpy(bytes.data(), s.data(), s.size());
	return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes)
{
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

json::object parse_object(std::string_view text)
{
	return json::parse(text).as_object();
}

std::string guid_of(const json::object& obj)
{
	return std::string(obj.at("__guid").as_string());
}

constexpr std::string_view k_parent_guid = "11111111-2222-4333-8444-555555555555";

struct guid_fixture : test_util::scene_env
{
	core::desc::desc_adapter_t adapter{ *rs, *descs };

	static res::tag res_tag() { return res::tag{ "res://t/cam.desc" }; }

	json::object save(const res::tag& tag, const std::shared_ptr<desc::desc_base>& inst)
	{
		return parse_object(text_of(adapter.serialize(tag, inst)));
	}
};

}

BOOST_FIXTURE_TEST_SUITE(DescGuidTests, guid_fixture)

BOOST_AUTO_TEST_CASE(GenerateGuid_IsLowercaseUuidV4AndUnique)
{
	const auto first = desc::generate_guid();
	BOOST_TEST(is_guid_v4(first));
	BOOST_TEST(desc::generate_guid() != first);
}

BOOST_AUTO_TEST_CASE(Copy_DoesNotInheritGuid)
{
	scn::camera_desc src;
	src.set_guid(desc::generate_guid());

	scn::camera_desc by_ctor(src);
	BOOST_TEST(by_ctor.get_guid().empty());

	scn::camera_desc by_assign;
	by_assign = src;
	BOOST_TEST(by_assign.get_guid().empty());

	scn::camera_desc by_copy_to;
	src.copy_to(by_copy_to);
	BOOST_TEST(by_copy_to.get_guid().empty());

	BOOST_TEST(is_guid_v4(src.get_guid()));
}

// #772: the ID of a file is its .resource; a save never makes up a __guid.
BOOST_AUTO_TEST_CASE(ResFile_WithoutGuid_SavesWithoutOne)
{
	const auto tag = res_tag();
	auto inst = std::dynamic_pointer_cast<desc::desc_base>(
		adapter.deserialize(tag, to_bytes(R"({"__type":"camera_desc","fov":45.0})")));
	BOOST_REQUIRE(inst);
	BOOST_TEST(inst->get_guid().empty());

	BOOST_TEST(!save(tag, inst).contains("__guid"));
	BOOST_TEST(inst->get_guid().empty());
	BOOST_TEST(!save(tag, descs->create_instance("camera_desc", tag)).contains("__guid"));
}

// #772: a file that has a __guid keeps it until the project's migration takes it out.
BOOST_AUTO_TEST_CASE(ResFile_GuidSurvivesSaveReloadSave)
{
	const auto tag = res_tag();
	const std::string guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
	auto inst = std::dynamic_pointer_cast<desc::desc_base>(
		adapter.deserialize(tag, to_bytes(R"({"__type":"camera_desc","__guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","fov":45.0})")));
	BOOST_REQUIRE(inst);

	BOOST_TEST(guid_of(save(tag, inst)) == guid);
	BOOST_TEST(guid_of(save(tag, inst)) == guid); // second save of the same instance

	auto reloaded = std::dynamic_pointer_cast<desc::desc_base>(
		adapter.deserialize(tag, to_bytes(text_of(adapter.serialize(tag, inst)))));
	BOOST_REQUIRE(reloaded);
	BOOST_TEST(reloaded->get_guid() == guid);
	BOOST_TEST(guid_of(save(tag, reloaded)) == guid);
}

BOOST_AUTO_TEST_CASE(MemoryFile_NeverWritesGuid)
{
	const auto tag = tag_of("cam.desc");
	auto inst = descs->create_instance("camera_desc", tag);
	BOOST_REQUIRE(inst);

	BOOST_TEST(!save(tag, inst).contains("__guid"));
	BOOST_TEST(inst->get_guid().empty());

	// Even an instance that already carries a guid (for example, a copy of a res file) writes none under memory://.
	inst->set_guid(desc::generate_guid());
	BOOST_TEST(!save(tag, inst).contains("__guid"));
}

BOOST_AUTO_TEST_CASE(ParentGuid_IsNotInheritedByChild)
{
	put("parent.desc", R"({"__type":"camera_desc","__guid":"11111111-2222-4333-8444-555555555555","fov":30.0})");

	const auto tag = res_tag();
	const auto bytes = to_bytes(R"({"__type":"camera_desc","__parent":"memory://t/parent.desc"})");
	const auto declared = adapter.collect_dependencies(tag, bytes);
	BOOST_REQUIRE(declared.size() == 1u);
	res::res_deps deps;
	deps.add(declared[0], rs->require_sync<desc::desc_base>(declared[0].target).get_sync());
	auto child = std::dynamic_pointer_cast<desc::desc_base>(adapter.deserialize(tag, bytes, deps));
	BOOST_REQUIRE(child);
	BOOST_TEST(child->get_guid().empty());

	BOOST_TEST(!save(tag, child).contains("__guid"));
}

BOOST_AUTO_TEST_CASE(LevelBuildJson_WritesGivenGuidOnly)
{
	std::vector<scn::world_desc> worlds;
	const auto with = scn::level_desc::build_json("L", worlds, *descs, k_parent_guid);
	BOOST_TEST(guid_of(with) == k_parent_guid);

	const auto without = scn::level_desc::build_json("L", worlds, *descs);
	BOOST_TEST(!without.contains("__guid"));
}

BOOST_AUTO_TEST_SUITE_END()
