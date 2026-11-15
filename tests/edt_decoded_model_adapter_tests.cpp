#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_decoded_model_adapter.h"
#include "edt_process_decoder.h"
#include "edt_filesystem_assimp_io.h"
#include <assimp/IOStream.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json/parse.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The editor's adapter of the models descs refer to (#925): the model goes to the decoder process with its content root
// as the boundary of what it may read and a time limit of its own, and the decoder's answer becomes the prefab. Nothing
// here parses a model: the decoder's steps are stand-ins.

namespace {

namespace fs = std::filesystem;

const res::tag k_model("res://models/m.obj");
const res::tag k_prefab("memory://__models/res/models/m.obj/m.prefab.desc");

struct adapter_env
{
	adapter_env()
	{
		dir.write("low/models/m.obj", "low");
		dir.write("content/models/m.obj", "o m");
		fs::create_directories(dir.path() / "memory");
		rs = std::make_unique<res::resource_system>(tasks, res::mount_table{
			res::mount{ .protocol = res::tag::default_protocol(), .roots = { dir.path() / "low", dir.path() / "content" } },
			res::mount{ .protocol = res::tag::memory, .roots = { dir.path() / "memory" } } });
		descs = std::make_unique<desc::desc_system>(*rs);
		rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs), std::ref(*descs));
		assembler = std::make_unique<scn::ecs_assembler>(*descs, *rs);
		scope.emplace(scn::register_scene_types(*descs, *assembler, skinning));
	}

	~adapter_env()
	{
		scope.reset();
		assembler.reset();
		descs.reset();
		rs.reset();
	}

	// A decoder that answers with an empty prefab under k_prefab, and records what it was asked.
	edt::decoded_model_adapter::decoder_steps answering()
	{
		edt::decoded_model_adapter::decoder_steps steps;
		steps.unavailable_reason = [] { return std::string{}; };
		steps.scan = [this](const edt::import_source& source, ds::task_context*) {
			scanned = source;
			scanned_at = std::chrono::steady_clock::now();
			edt::model_file_scan found;
			found.files.push_back({ dir.path() / "content" / "textures" / "t.png", 3 });
			return found;
		};
		steps.import = [this](const edt::import_source& source, ds::task_context*) {
			imported = source;
			imported_allowed = source.allowed != nullptr ? *source.allowed : std::vector<edt::referenced_file>{};
			edt::import_intermediate answer;
			answer.prefab_tag = k_prefab;
			answer.prefab_json = boost::json::parse(R"({ "__type": "prefab_desc" })").as_object();
			descs->migrate_tree(answer.prefab_json, "test");
			return answer;
		};
		return steps;
	}

	std::vector<std::byte> bytes() const { return { std::byte{ 'o' }, std::byte{ ' ' }, std::byte{ 'm' } }; }

	test_util::temp_dir dir{ "decoded_model_adapter" };
	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> rs;
	std::unique_ptr<desc::desc_system> descs;
	std::unique_ptr<scn::ecs_assembler> assembler;
	rnd::skinning_manager skinning;
	std::optional<scn::registration_scope> scope;

	std::optional<edt::import_source> scanned;
	std::optional<edt::import_source> imported;
	std::vector<edt::referenced_file> imported_allowed;
	std::chrono::steady_clock::time_point scanned_at{};
};

std::string error_of(const edt::decoded_model_adapter& adapter, const res::tag& tag, const std::vector<std::byte>& bytes)
{
	try {
		adapter.deserialize(tag, bytes);
	} catch (const std::runtime_error& e) {
		return e.what();
	}
	return {};
}

}

BOOST_FIXTURE_TEST_SUITE(EdtDecodedModelAdapterTests, adapter_env)

BOOST_AUTO_TEST_CASE(TheModelIsFoundInTheHighestRootThatHasIt)
{
	const auto found = edt::decoded_model_adapter::locate({ dir.path() / "low", dir.path() / "content" }, k_model);
	BOOST_REQUIRE(found.has_value());
	BOOST_TEST(fs::equivalent(found->file, dir.path() / "content" / "models" / "m.obj"));
	BOOST_TEST(fs::equivalent(found->root, dir.path() / "content"));

	const auto only_low = edt::decoded_model_adapter::locate({ dir.path() / "low" }, k_model);
	BOOST_REQUIRE(only_low.has_value());
	BOOST_TEST(fs::equivalent(only_low->root, dir.path() / "low"));

	BOOST_TEST(!edt::decoded_model_adapter::locate({ dir.path() / "low", dir.path() / "content" }, res::tag("res://models/none.obj")).has_value());
}

BOOST_AUTO_TEST_CASE(EachModelPathHasAMemoryFolderOfItsOwn)
{
	BOOST_TEST(edt::decoded_model_adapter::memory_folder_of(res::tag("res://objects/robot/gen_robot.glb")) == "__models/res/objects/robot/gen_robot.glb/");
	BOOST_TEST(edt::decoded_model_adapter::memory_folder_of(res::tag("res://objects/robot/gen_robot.fbx")) == "__models/res/objects/robot/gen_robot.fbx/");
	BOOST_TEST(edt::decoded_model_adapter::memory_folder_of(res::tag("res://gen_robot.glb")) == "__models/res/gen_robot.glb/");
	BOOST_TEST(edt::decoded_model_adapter::memory_folder_of(res::tag("engine://a/gen_robot.glb")) == "__models/engine/a/gen_robot.glb/");
}

BOOST_AUTO_TEST_CASE(TheDecoderGetsTheContentRootAndItsOwnTimeLimit_AndItsAnswerIsThePrefab)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, answering());
	const std::vector<std::byte> model = bytes();
	const auto loaded = adapter.deserialize(k_model, model);

	BOOST_REQUIRE(scanned.has_value());
	BOOST_REQUIRE(imported.has_value());
	BOOST_TEST(fs::equivalent(scanned->path, dir.path() / "content" / "models" / "m.obj"));
	BOOST_TEST(fs::equivalent(scanned->boundary, dir.path() / "content"));
	BOOST_TEST(scanned->memory_folder == "__models/res/models/m.obj/");
	BOOST_TEST((scanned->preloaded == &model));
	BOOST_TEST((scanned->allowed == nullptr));
	BOOST_TEST((scanned->deadline > scanned_at && scanned->deadline <= scanned_at + edt::decoded_model_adapter::k_deadline));
	BOOST_TEST((edt::decoded_model_adapter::k_deadline == std::chrono::seconds(10)));
	BOOST_TEST(imported->boundary == scanned->boundary);
	BOOST_TEST(imported->memory_folder == scanned->memory_folder);
	BOOST_REQUIRE_EQUAL(imported_allowed.size(), 1u);
	BOOST_TEST(imported_allowed[0].path == dir.path() / "content" / "textures" / "t.png");

	BOOST_TEST(std::dynamic_pointer_cast<scn::prefab_desc>(loaded) != nullptr);
	BOOST_TEST((rs->resolve_alias(k_model) == k_prefab));
}

BOOST_AUTO_TEST_CASE(WithoutTheDecoder_TheLoadFailsWithItsReason_AndNothingIsParsed)
{
	edt::decoded_model_adapter::decoder_steps steps = answering();
	steps.unavailable_reason = [] { return std::string("the model decoder 'bin/snk_model_decoder' was not found next to the editor"); };
	const edt::decoded_model_adapter adapter(*descs, *rs, std::move(steps));

	const std::string error = error_of(adapter, k_model, bytes());
	BOOST_TEST(error.find("res://models/m.obj") != std::string::npos);
	BOOST_TEST(error.find("was not found next to the editor") != std::string::npos);
	BOOST_TEST(!scanned.has_value());
	BOOST_TEST(!imported.has_value());
}

BOOST_AUTO_TEST_CASE(AModelThatIsNoFileOnTheDisk_GoesToTheDecoderAsItsBytesAlone)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, answering());
	const std::vector<std::byte> model = bytes();
	const auto loaded = adapter.deserialize(res::tag("res://models/none.obj"), model);

	BOOST_REQUIRE(scanned.has_value());
	BOOST_TEST(!scanned->on_disk);
	BOOST_TEST(scanned->boundary.empty());
	BOOST_TEST(scanned->path.filename() == fs::path("none.obj"));
	BOOST_TEST((scanned->preloaded == &model));
	BOOST_REQUIRE(imported.has_value());
	BOOST_TEST(!imported->on_disk);
	BOOST_TEST(std::dynamic_pointer_cast<scn::prefab_desc>(loaded) != nullptr);
}

BOOST_AUTO_TEST_CASE(AModelOnTheDisk_GoesToTheDecoderAsAFile)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, answering());
	adapter.deserialize(k_model, bytes());
	BOOST_REQUIRE(scanned.has_value());
	BOOST_TEST(scanned->on_disk);
}

BOOST_AUTO_TEST_CASE(WhatTheDecoderRefusesOrFailsOn_FailsTheLoadNamingTheModel)
{
	{
		edt::decoded_model_adapter::decoder_steps steps = answering();
		steps.scan = [](const edt::import_source&, ds::task_context*) {
			edt::model_file_scan found;
			found.error = "the model refers to '../../secret.png': outside the content root of the model";
			return found;
		};
		const edt::decoded_model_adapter adapter(*descs, *rs, std::move(steps));
		const std::string error = error_of(adapter, k_model, bytes());
		BOOST_TEST(error.find("res://models/m.obj") != std::string::npos);
		BOOST_TEST(error.find("outside the content root") != std::string::npos);
		BOOST_TEST(!imported.has_value());
	}
	{
		edt::decoded_model_adapter::decoder_steps steps = answering();
		steps.import = [](const edt::import_source&, ds::task_context*) -> edt::import_intermediate {
			throw std::runtime_error("the model took too long to decode");
		};
		const edt::decoded_model_adapter adapter(*descs, *rs, std::move(steps));
		const std::string error = error_of(adapter, k_model, bytes());
		BOOST_TEST(error.find("res://models/m.obj") != std::string::npos);
		BOOST_TEST(error.find("took too long") != std::string::npos);
	}
	{
		edt::decoded_model_adapter::decoder_steps steps = answering();
		steps.import = [](const edt::import_source&, ds::task_context*) {
			edt::import_intermediate failed;
			failed.error = "ASSIMP error: no mesh";
			return failed;
		};
		const edt::decoded_model_adapter adapter(*descs, *rs, std::move(steps));
		BOOST_TEST(error_of(adapter, k_model, bytes()).find("ASSIMP error: no mesh") != std::string::npos);
	}
	BOOST_TEST((rs->resolve_alias(k_model) == k_model));
}

BOOST_AUTO_TEST_CASE(WhatAnEarlierLoadLeftInTheModelsMemoryFolder_IsGoneBeforeTheImport_AndAModelOfTheSameStemKeepsItsOwn)
{
	dir.write("memory/__models/res/models/m.obj/m.prefab.desc", "{}");
	dir.write("memory/__models/res/models/m.obj/textures/old.png", "png");
	dir.write("memory/__models/res/models/m.fbx/m.prefab.desc", "{}");
	const res::tag old_prefab("memory://__models/res/models/m.obj/m.prefab.desc");
	const res::tag old_texture("memory://__models/res/models/m.obj/textures/old.png");
	const res::tag other("memory://__models/res/models/m.fbx/m.prefab.desc");
	BOOST_REQUIRE(rs->exists(old_prefab));
	BOOST_REQUIRE(rs->exists(old_texture));

	edt::decoded_model_adapter::decoder_steps steps = answering();
	std::optional<bool> left_over;
	steps.import = [&, answer = steps.import](const edt::import_source& source, ds::task_context* task) {
		left_over = rs->exists(old_prefab) || rs->exists(old_texture);
		return answer(source, task);
	};
	const edt::decoded_model_adapter adapter(*descs, *rs, std::move(steps));
	adapter.deserialize(k_model, bytes());

	BOOST_REQUIRE(left_over.has_value());
	BOOST_TEST(!*left_over);
	BOOST_TEST(rs->exists(other));
}

BOOST_AUTO_TEST_SUITE_END()

#if defined(SNK_MODEL_DECODER_PATH)

// The same loads through the real decoder process, for a model that is no file on the disk (#925): a self-contained .glb
// decodes; a model that names another file, or bytes that are no model, fail the load with the reason.
namespace {

struct real_decoder_env : adapter_env
{
	edt::decoded_model_adapter adapter()
	{
		edt::decode_runner_config config;
		config.decoder = SNK_MODEL_DECODER_PATH;
		return edt::decoded_model_adapter(*descs, *rs, edt::decoded_model_adapter::steps_of(std::make_shared<edt::process_decoder>(*rs, config)));
	}

	static std::vector<std::byte> text(std::string_view content)
	{
		std::vector<std::byte> out(content.size());
		for (std::size_t i = 0; i < content.size(); ++i) out[i] = static_cast<std::byte>(content[i]);
		return out;
	}
};

std::vector<std::byte> file_bytes(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return real_decoder_env::text(content);
}

}

BOOST_FIXTURE_TEST_SUITE(EdtDecodedModelAdapterRealDecoderTests, real_decoder_env)

BOOST_AUTO_TEST_CASE(AGlbFromMemory_IsDecoded)
{
	const std::vector<std::byte> robot = file_bytes(fs::path(SNK_TEST_PROJECT_DIR) / "res" / "objects" / "robot" / "gen_robot.glb");
	BOOST_REQUIRE(!robot.empty());
	const auto loaded = adapter().deserialize(res::tag("res://ram/gen_robot.glb"), robot);
	BOOST_TEST(std::dynamic_pointer_cast<scn::prefab_desc>(loaded) != nullptr);
	BOOST_TEST((rs->resolve_alias(res::tag("res://ram/gen_robot.glb")) == res::tag("memory://__models/res/ram/gen_robot.glb/gen_robot.prefab.desc")));
}

BOOST_AUTO_TEST_CASE(AModelFromMemoryThatNamesAnotherFile_FailsTheLoad)
{
	const std::string gltf = R"json({"asset":{"version":"2.0"},"buffers":[{"uri":"m.bin","byteLength":36}],
		"bufferViews":[{"buffer":0,"byteLength":36}],
		"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]}],
		"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})json";
	const std::string obj = "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl red\nf 1 2 3\n";

	struct named_file { const char* tag; std::string content; const char* named; };
	for (const named_file& model : { named_file{ "res://ram/m.gltf", gltf, "m.bin': the model is not a file on the disk" },
		named_file{ "res://ram/m.obj", obj, "m.mtl': the model is not a file on the disk" } }) {
		BOOST_TEST_CONTEXT(model.tag) {
			const std::string error = error_of(adapter(), res::tag(model.tag), text(model.content));
			BOOST_TEST(error.find(model.tag) != std::string::npos);
			BOOST_CHECK_MESSAGE(error.find(model.named) != std::string::npos, error);
			BOOST_CHECK_MESSAGE(error.find("so it cannot read other files") != std::string::npos, error);
		}
	}
}

BOOST_AUTO_TEST_CASE(BytesFromMemoryThatAreNoModel_FailTheLoad)
{
	const std::string error = error_of(adapter(), res::tag("res://ram/broken.glb"), text("glTF this is not a model"));
	BOOST_TEST(error.find("res://ram/broken.glb") != std::string::npos);
	// What the decoder answered: its scan could not read the bytes.
	BOOST_CHECK_MESSAGE(error.find("is not loaded: the model cannot be read") != std::string::npos, error);
}

// The reading side of the decoder for a model given only as bytes (#925): the model itself is served, every other name is
// refused, also when a file of that name lies next to the path.
BOOST_AUTO_TEST_CASE(TheAssimpIoOfAModelGivenOnlyAsBytes_ServesTheModelAndRefusesEveryOtherFile)
{
	dir.write("ram/m.mtl", "newmtl red\n");
	const fs::path model = dir.path() / "ram" / "m.obj";
	const std::vector<std::byte> bytes = text("v 0 0 0\n");
	edt::model_file_access access(model, {}, nullptr, {}, false);
	edt::filesystem_assimp_io io(model, bytes, &access);

	BOOST_TEST(io.Exists(model.string().c_str()));
	Assimp::IOStream* stream = io.Open(model.string().c_str(), "rb");
	BOOST_REQUIRE(stream != nullptr);
	BOOST_TEST(stream->FileSize() == bytes.size());
	io.Close(stream);

	const std::string side = (dir.path() / "ram" / "m.mtl").string();
	for (const std::string& name : { std::string("m.mtl"), side }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_CHECK_THROW(io.Exists(name.c_str()), std::runtime_error);
			BOOST_CHECK_THROW(io.Open(name.c_str(), "rb"), std::runtime_error);
		}
	}
	BOOST_TEST(access.files().empty());
}

BOOST_AUTO_TEST_SUITE_END()

#endif
