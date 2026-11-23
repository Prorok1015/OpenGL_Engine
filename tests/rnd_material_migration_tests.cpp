#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/render/rnd_material_migration.h>
#include <snk/desc/desc_system.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// material_desc v1 -> v2 (#1200): the stage tags become a reference to an engine .shader.desc, or the load is refused.

namespace json = boost::json;

namespace {

constexpr std::string_view engine_base_guid = "f90d3c03-865c-4193-aa09-cb2f98315634";

struct migrated
{
	json::object material;
	std::vector<desc::migration_warning> warnings;
};

migrated migrate(std::string_view text, desc::migration_id_lookup id_of = {})
{
	migrated out{ json::parse(text).as_object(), {} };
	desc::migration_context context("material_desc", 1, "materials/m.desc", &out.warnings, std::move(id_of));
	rnd::migrate_material_v1(out.material, context);
	return out;
}

desc::migration_id_lookup ids_answering(std::string_view guid)
{
	return [id = res::asset_id_from_text(guid)](std::string_view path) -> std::optional<res::asset_id> {
		return path == "res://base_material.desc" ? id : std::nullopt;
	};
}

bool refused(std::string_view text, desc::migration_id_lookup id_of = {})
{
	try {
		migrate(text, std::move(id_of));
	} catch (const std::runtime_error& error) {
		BOOST_TEST(std::string_view{ error.what() }.find("project migration") != std::string_view::npos);
		return true;
	}
	return false;
}

std::string shader_of(const json::object& material)
{
	BOOST_REQUIRE(material.contains("shader"));
	for (const char* key : { "shader_vertex", "shader_fragment", "shader_geometry" }) {
		BOOST_TEST(!material.contains(key));
	}
	return std::string{ material.at("shader").as_string() };
}

} // namespace

BOOST_AUTO_TEST_SUITE(RndMaterialMigrationTests)

BOOST_AUTO_TEST_CASE(TheEnginesPairs_BecomeTheirShaderDesc_WhicheverWayAStageIsWritten)
{
	BOOST_TEST(shader_of(migrate(R"({"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag","shader_geometry":null})").material)
		== "res://shaders/scene.shader.desc");
	BOOST_TEST(shader_of(migrate(R"({"shader_vertex":"engine://shaders/sky.vert","shader_fragment":"engine://shaders/sky.frag"})").material)
		== "res://shaders/sky.shader.desc");
	BOOST_TEST(shader_of(migrate(R"({"shader_vertex":{"guid":"6a0e2f1c-3b5d-4e7f-9a1b-2c3d4e5f6a7b","path":"res://shaders/scene.vert"},
		"shader_fragment":{"path":"res://shaders/mix_opaque_trans_scene.frag"},"shader_geometry":""})").material)
		== "res://shaders/mix_opaque_trans_scene.shader.desc");
}

BOOST_AUTO_TEST_CASE(TheOtherKeysStay)
{
	const migrated out = migrate(R"({"__type":"material_desc","__guid":"0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c","defines":["A"],
		"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag","queue":"mix"})");
	BOOST_TEST(out.material.at("queue").as_string() == "mix");
	BOOST_TEST(out.material.at("defines").as_array().size() == 1u);
	BOOST_TEST(out.material.at("__guid").as_string() == "0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c");
	BOOST_TEST(out.warnings.empty());
}

BOOST_AUTO_TEST_CASE(AMaterialWithoutStages_KeepsInheriting)
{
	const migrated out = migrate(R"({"__parent":"res://base_material.desc","queue":"transparent"})");
	BOOST_TEST(!out.material.contains("shader"));
	BOOST_TEST(out.material.size() == 2u);
}

BOOST_AUTO_TEST_CASE(EveryStageEmpty_UnderAParent_TakesTheParentsShader_WithAWarning)
{
	const migrated out = migrate(R"({"__parent":"res://base_material.desc","shader_vertex":null,"shader_fragment":null,"shader_geometry":null})");
	BOOST_TEST(!out.material.contains("shader"));
	BOOST_TEST(!out.material.contains("shader_vertex"));
	BOOST_TEST(out.warnings.size() == 1u);
}

BOOST_AUTO_TEST_CASE(OnlyAFragment_OverTheEnginesBaseMaterial_TakesItsVertexStage)
{
	constexpr std::string_view imported = R"({"__parent":"res://base_material.desc","shader_fragment":"res://shaders/mix_opaque_trans_scene.frag"})";
	BOOST_TEST(shader_of(migrate(imported, ids_answering(engine_base_guid)).material) == "res://shaders/mix_opaque_trans_scene.shader.desc");

	// A reference with the engine's ID is asked about too: a project file of the same path wins over it (R28).
	const std::string by_id = std::format(R"({{"__parent":{{"guid":"{}","path":"res://base_material.desc"}},"shader_fragment":"res://shaders/scene.frag"}})",
		engine_base_guid);
	BOOST_TEST(shader_of(migrate(by_id, ids_answering(engine_base_guid)).material) == "res://shaders/scene.shader.desc");
	BOOST_TEST(refused(by_id));
	BOOST_TEST(refused(by_id, ids_answering("0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c")));
}

BOOST_AUTO_TEST_CASE(OnlyAFragment_OverAnotherParent_IsRefused)
{
	constexpr std::string_view imported = R"({"__parent":"res://base_material.desc","shader_fragment":"res://shaders/mix_opaque_trans_scene.frag"})";
	// Nobody to ask, a project file of the same path, or one with another ID.
	BOOST_TEST(refused(imported));
	BOOST_TEST(refused(imported, ids_answering("0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c")));
	BOOST_TEST(refused(imported, [](std::string_view) { return std::optional<res::asset_id>{}; }));
	// A parent between them.
	BOOST_TEST(refused(R"({"__parent":"res://materials/mid.desc","shader_fragment":"res://shaders/scene.frag"})", ids_answering(engine_base_guid)));
	BOOST_TEST(refused(R"({"__parent":{"guid":"0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c","path":"res://base_material.desc"},"shader_fragment":"res://shaders/scene.frag"})",
		ids_answering(engine_base_guid)));
}

BOOST_AUTO_TEST_CASE(AStageReferencedByIdOnly_IsNoEmptyStage_ItIsRefused)
{
	// Taken for empty, the stages would be dropped and the material saved as v2 without a shader.
	BOOST_TEST(refused(R"({"__parent":"res://base_material.desc","shader_vertex":{"guid":"6a0e2f1c-3b5d-4e7f-9a1b-2c3d4e5f6a7b"},
		"shader_fragment":null,"shader_geometry":null})", ids_answering(engine_base_guid)));
	BOOST_TEST(refused(R"({"shader_vertex":{"guid":"6a0e2f1c-3b5d-4e7f-9a1b-2c3d4e5f6a7b"},"shader_fragment":"res://shaders/scene.frag"})"));
}

BOOST_AUTO_TEST_CASE(StagesTheEngineHasNoShaderFor_AreRefused)
{
	BOOST_TEST(refused(R"({"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/custom.frag"})"));
	BOOST_TEST(refused(R"({"shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag","shader_geometry":"res://shaders/obj.geom"})"));
	BOOST_TEST(refused(R"({"shader_vertex":"res://shaders/sky.vert"})"));
	BOOST_TEST(refused(R"({"shader_vertex":"res://shaders/sky.vert","shader_fragment":"res://shaders/scene.frag"})"));
}

BOOST_AUTO_TEST_CASE(WhatIsWritten_DoesNotDependOnTheAnswer_OnlyWhetherItConverts)
{
	constexpr std::string_view full = R"({"__parent":"res://base_material.desc","shader_vertex":"res://shaders/scene.vert","shader_fragment":"res://shaders/scene.frag"})";
	const json::object asked = migrate(full, ids_answering(engine_base_guid)).material;
	const json::object other = migrate(full, ids_answering("0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c")).material;
	const json::object nobody = migrate(full).material;
	BOOST_TEST(json::serialize(asked) == json::serialize(other));
	BOOST_TEST(json::serialize(asked) == json::serialize(nobody));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// The project's asset index as it serves res://base_material.desc: the engine's file, or one of the project.
struct base_material_index final : res::asset_index
{
	std::string_view guid;
	std::optional<res::asset_id> id_of(const res::tag& tag) const override
	{
		return tag.view() == "res://base_material.desc" ? res::asset_id_from_text(guid) : std::nullopt;
	}
	std::optional<res::tag> path_of(const res::asset_id&) const override { return std::nullopt; }
};

constexpr std::string_view k_imported = R"({"__type":"material_desc","__version":1,"__parent":"res://base_material.desc",
	"defines":["LIGHTS_ENABLED"],"shader_fragment":"res://shaders/mix_opaque_trans_scene.frag","queue":"mix"})";

} // namespace

// The step as the engine runs it: registered with the scene types, asked through migrate_tree, answered by the asset index.
BOOST_FIXTURE_TEST_SUITE(RndMaterialMigrationThroughTheDescSystemTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(AnImportedMaterial_ConvertsWithTheEnginesBaseInTheIndex_AndIsRefusedOtherwise)
{
	BOOST_TEST(descs->current_version("material_desc") == 2);

	auto index = std::make_shared<base_material_index>();
	index->guid = engine_base_guid;
	rs->set_asset_index(index);
	json::object converted = json::parse(k_imported).as_object();
	descs->migrate_tree(converted, "res://models/robot/robot.mat.desc");
	BOOST_TEST(converted.at("shader").as_string() == "res://shaders/mix_opaque_trans_scene.shader.desc");
	BOOST_TEST(converted.at("__version").as_int64() == 2);
	BOOST_TEST(!converted.contains("shader_fragment"));

	// A project file of the same path.
	auto project_file = std::make_shared<base_material_index>();
	project_file->guid = "0b6f6b1e-5f0a-4a8e-9b3c-1d2e3f4a5b6c";
	rs->set_asset_index(project_file);
	json::object shadowed = json::parse(k_imported).as_object();
	BOOST_CHECK_THROW(descs->migrate_tree(shadowed, "res://models/robot/robot.mat.desc"), std::runtime_error);

	// No index (a tool, a runtime without one).
	rs->set_asset_index(nullptr);
	json::object no_index = json::parse(k_imported).as_object();
	BOOST_CHECK_THROW(descs->migrate_tree(no_index, "res://models/robot/robot.mat.desc"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
