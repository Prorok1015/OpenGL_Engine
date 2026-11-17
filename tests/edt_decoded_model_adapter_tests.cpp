#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_decoded_model_adapter.h"
#include "edt_process_decoder.h"
#include "edt_model_file_source.h"
#include "edt_file_identity.h"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/resource/res_mount_table.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
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

std::vector<std::byte> text_bytes(std::string_view text)
{
	std::vector<std::byte> out(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
	return out;
}

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

	// As the resource system runs an adapter: in a compute task (resource_system::read_now refuses the main thread).
	std::shared_ptr<res::resource_entry> load(const edt::decoded_model_adapter& adapter, const res::tag& tag,
		const std::vector<std::byte>& bytes)
	{
		std::shared_ptr<res::resource_entry> loaded;
		test_util::in_task(tasks, [&] { loaded = adapter.deserialize(tag, bytes); });
		return loaded;
	}

	std::string error_of(const edt::decoded_model_adapter& adapter, const res::tag& tag,
		const std::vector<std::byte>& bytes)
	{
		try {
			load(adapter, tag, bytes);
		} catch (const std::runtime_error& e) {
			return e.what();
		}
		return {};
	}

	std::optional<edt::import_source> scanned;
	std::optional<edt::import_source> imported;
	std::vector<edt::referenced_file> imported_allowed;
	std::chrono::steady_clock::time_point scanned_at{};
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDecodedModelAdapterTests, adapter_env)

// #996: the file the resolver reads the model from, only while it holds the bytes the load got.
BOOST_AUTO_TEST_CASE(TheModelIsTheFileOfTheHighestRootThatHasIt_WhileItHoldsTheBytes)
{
	std::string differs;
	const auto found = rs->disk_file_holding(k_model, bytes(), differs);
	BOOST_REQUIRE(found.has_value());
	BOOST_TEST(fs::equivalent(found->file, dir.path() / "content" / "models" / "m.obj"));
	BOOST_TEST(fs::equivalent(found->root, dir.path() / "content"));
	BOOST_TEST(differs.empty());

	// The low root's file is not the one read: the bytes of the low file are not taken from the high one.
	BOOST_TEST(!rs->disk_file_holding(k_model, text_bytes("low"), differs).has_value());
	BOOST_TEST(!differs.empty());

	differs.clear();
	BOOST_TEST(!rs->disk_file_holding(res::tag("res://models/none.obj"), bytes(), differs).has_value());
	BOOST_TEST(differs.empty());
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
	const auto loaded = load(adapter, k_model, model);

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
	const auto loaded = load(adapter, res::tag("res://models/none.obj"), model);

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
	load(adapter, k_model, bytes());
	BOOST_REQUIRE(scanned.has_value());
	BOOST_TEST(scanned->on_disk);
}

// #996: memory:// holds what was made at runtime; on the disk in the editor, still its models go by their bytes.
BOOST_AUTO_TEST_CASE(AMemoryModel_GoesToTheDecoderAsItsBytesAlone_ThoughItsFolderIsOnTheDisk)
{
	dir.write("memory/models/x.obj", "o m");
	const edt::decoded_model_adapter adapter(*descs, *rs, answering());
	load(adapter, res::tag("memory://models/x.obj"), bytes());
	BOOST_REQUIRE(scanned.has_value());
	BOOST_TEST(!scanned->on_disk);
	BOOST_TEST(scanned->boundary.empty());
}

// #996: a resolver that reads no files serves the bytes; a file of the same name in the mount's root is not the model.
BOOST_AUTO_TEST_CASE(AModelFromAResolverThatReadsNoFiles_GoesAsItsBytes_ThoughAFileOfItsNameIsOnTheDisk)
{
	rs->unregistrate_resolver(res::tag::default_protocol());
	rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
	const edt::decoded_model_adapter adapter(*descs, *rs, answering());
	load(adapter, k_model, bytes());
	BOOST_REQUIRE(scanned.has_value());
	BOOST_TEST(!scanned->on_disk);
	BOOST_TEST(scanned->boundary.empty());
}

// #996: the file changed (or is another one) after the bytes were read: it is not the model, which goes by its bytes.
BOOST_AUTO_TEST_CASE(AModelWhoseFileHoldsOtherBytes_GoesAsItsBytes_AndThatIsWarned)
{
	std::vector<std::string> warned;
	const engine::log_sink_handle sink = engine::add_log_sink(
		[&warned](std::string_view, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) warned.emplace_back(message);
		});
	const edt::decoded_model_adapter adapter(*descs, *rs, answering());
	load(adapter, k_model, text_bytes("o x"));
	BOOST_REQUIRE(scanned.has_value());
	BOOST_TEST(!scanned->on_disk);
	const auto named = std::count_if(warned.begin(), warned.end(), [](const std::string& line) {
		return line.find("res://models/m.obj") != std::string::npos && line.find("bytes alone") != std::string::npos;
	});
	BOOST_TEST(named == 1);
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
	load(adapter, k_model, bytes());

	BOOST_REQUIRE(left_over.has_value());
	BOOST_TEST(!*left_over);
	BOOST_TEST(rs->exists(other));
}

BOOST_AUTO_TEST_SUITE_END()

// A model loaded again with the same bytes is not decoded again (#999). The stand-in decoder stores an import that passes
// the checks of a real answer (a prefab and a geometry in the model's memory:// folder) and lists one side file it read.
namespace {

const res::tag k_geom("memory://__models/res/models/m.obj/m.geom.desc");
const fs::path k_side_file = fs::path("content") / "models" / "m.mtl";

struct cache_env : adapter_env
{
	cache_env() { dir.write(k_side_file.generic_string(), "newmtl a"); }

	edt::decoded_model_adapter::decoder_steps decoding()
	{
		edt::decoded_model_adapter::decoder_steps steps;
		steps.unavailable_reason = [] { return std::string{}; };
		steps.version = [this] { return version; };
		steps.scan = [this](const edt::import_source&, ds::task_context*) {
			++scans;
			const fs::path side = dir.path() / k_side_file;
			const std::vector<std::byte> bytes = text_bytes("newmtl a");
			edt::referenced_file file;
			file.path = side;
			file.size = bytes.size();
			file.digest = edt::digest_of(bytes);
			file.hashed = true;
			edt::model_file_scan found;
			found.files.push_back(file);
			found.total_bytes = file.size;
			return found;
		};
		steps.import = [this](const edt::import_source&, ds::task_context*) {
			++imports;
			const std::vector<std::byte> prefab = text_bytes(R"({"__type":"prefab_desc"})");
			const std::vector<std::byte> geom = text_bytes(R"({"__type":"geometry_desc","__parent":"res://base_geometry.desc"})");
			BOOST_REQUIRE(rs->store_sync(k_prefab, prefab));
			BOOST_REQUIRE(rs->store_sync(k_geom, geom));
			edt::import_intermediate answer;
			answer.prefab_tag = k_prefab;
			answer.geom_tag = k_geom;
			answer.created_tags = { k_prefab, k_geom };
			if (with_digests) answer.created_digests = { { k_prefab, edt::digest_of(prefab) }, { k_geom, edt::digest_of(geom) } };
			answer.all_tags = answer.created_tags;
			answer.prefab_json = boost::json::parse(R"({ "__type": "prefab_desc" })").as_object();
			descs->migrate_tree(answer.prefab_json, "test");
			return answer;
		};
		return steps;
	}

	std::string version = "decoder-1";
	bool with_digests = true;
	int scans = 0;
	int imports = 0;
};

struct warnings
{
	warnings()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) lines.push_back(std::format("{}: {}", category, message));
		});
	}

	std::size_t count(std::string_view part) const
	{
		std::size_t n = 0;
		for (const std::string& line : lines) {
			if (line.find(part) != std::string::npos) ++n;
		}
		return n;
	}

	std::vector<std::string> lines;
	engine::log_sink_handle sink;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDecodedModelCacheTests, cache_env)

BOOST_AUTO_TEST_CASE(TheSameBytesLoadedAgain_AreNotDecodedAgain_AndGiveThePrefab)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	BOOST_TEST(std::dynamic_pointer_cast<scn::prefab_desc>(load(adapter, k_model, bytes())) != nullptr);
	const warnings warned;
	BOOST_TEST(std::dynamic_pointer_cast<scn::prefab_desc>(load(adapter, k_model, bytes())) != nullptr);
	BOOST_TEST(scans == 1);
	BOOST_TEST(imports == 1);
	BOOST_TEST((rs->resolve_alias(k_model) == k_prefab));
	BOOST_TEST(warned.count("edt/decoder") == 0u);
}

BOOST_AUTO_TEST_CASE(OtherBytesOfTheModel_AreDecoded)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	load(adapter, k_model, text_bytes("o n"));
	BOOST_TEST(imports == 2);
	load(adapter, k_model, text_bytes("o n"));
	BOOST_TEST(imports == 2, "the new bytes are the ones reused now");
}

BOOST_AUTO_TEST_CASE(ASideFileTheScanListedThatChanged_IsDecodedAgain)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	dir.write(k_side_file.generic_string(), "newmtl b");
	load(adapter, k_model, bytes());
	BOOST_TEST(scans == 2);
	BOOST_TEST(imports == 2);
}

BOOST_AUTO_TEST_CASE(AnotherBuildOfTheDecoder_DecodesAgain)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	version = "decoder-2";
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 2);
}

BOOST_AUTO_TEST_CASE(WithoutTheVersionOfTheDecoder_NothingIsReused)
{
	version.clear();
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 2);
}

BOOST_AUTO_TEST_CASE(AResourceChangedInMemory_IsRefusedAsADecodersAnswerWouldBe_AndTheModelIsDecodedAgain)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	BOOST_REQUIRE(rs->store_sync(k_geom, text_bytes(R"({"__type":"geometry_desc","__parent":"res://levels/secret.desc"})")));
	const warnings warned;
	BOOST_TEST(std::dynamic_pointer_cast<scn::prefab_desc>(load(adapter, k_model, bytes())) != nullptr);
	BOOST_TEST(imports == 2);
	BOOST_TEST(warned.count("the earlier decode of 'res://models/m.obj' was refused") == 1u);
}

BOOST_AUTO_TEST_CASE(AResourceSwappedForOneThatPassesTheChecks_IsNotTaken_AndTheModelIsDecodedAgain)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	BOOST_REQUIRE(rs->store_sync(k_geom, text_bytes(R"({"__type":"geometry_desc","__parent":"res://base_geometry.desc","name":"other"})")));
	const warnings warned;
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 2);
	BOOST_TEST(warned.count("is not what the decoder stored") == 1u);
}

BOOST_AUTO_TEST_CASE(AnImportWithoutTheDigestsOfItsResources_IsNotReused)
{
	with_digests = false;
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 2);
}

BOOST_AUTO_TEST_CASE(TheSameBytesFoundInAnotherRoot_AreDecodedAgain)
{
	// Same tag, same folder, same bytes; another file and content root, so another boundary for its side files.
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	fs::remove(dir.path() / "content" / "models" / "m.obj");
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 2);
}

BOOST_AUTO_TEST_CASE(AResourceGoneFromMemory_IsDecodedAgain)
{
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	rs->erase(k_geom);
	BOOST_REQUIRE(!rs->exists(k_geom));
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 2);
	BOOST_TEST(rs->exists(k_geom));
}

BOOST_AUTO_TEST_CASE(TheVersionOfTheDecoderProcess_IsTheSha256OfItsFile_AndEmptyWithoutIt)
{
	const fs::path file = dir.write("bin/snk_model_decoder", "decoder bytes");
	edt::decode_runner_config config;
	config.decoder = file;
	const edt::process_decoder decoder(*rs, *descs, config);
	BOOST_TEST(decoder.version() == cmn::sha256_file_hex(file).value());

	config.decoder = dir.path() / "bin" / "missing";
	const edt::process_decoder missing(*rs, *descs, config);
	const warnings warned;
	BOOST_TEST(missing.version().empty());
	BOOST_TEST(missing.version().empty());
	BOOST_TEST(warned.count("cannot be read") == 1u, "one warning, however often it is asked");
}

// #1086: an earlier import is read back through the resolver that serves memory://, not from its folder on the disk.
BOOST_AUTO_TEST_CASE(AnEarlierImport_IsReadBackThroughTheResolverOfMemory)
{
	rs->unregistrate_resolver(res::tag::memory);
	rs->registrate_resolver<test_util::ram_resolver>(res::tag::memory);
	const edt::decoded_model_adapter adapter(*descs, *rs, decoding());
	load(adapter, k_model, bytes());
	load(adapter, k_model, bytes());
	BOOST_TEST(imports == 1);
}

// #1086: a resolver of memory:// that cannot list its files gets what the last decode made erased, and a warning.
BOOST_AUTO_TEST_CASE(UnderAMemoryResolverThatCannotList_WhatTheLastDecodeMadeIsErased_AndThatIsWarned)
{
	rs->unregistrate_resolver(res::tag::memory);
	rs->registrate_resolver<test_util::ram_resolver>(res::tag::memory);
	edt::decoded_model_adapter::decoder_steps steps = decoding();
	bool geom_there_at_import = true;
	steps.import = [&, real_import = steps.import](const edt::import_source& source, ds::task_context* context) {
		geom_there_at_import = rs->exists(k_geom);
		return real_import(source, context);
	};
	const edt::decoded_model_adapter adapter(*descs, *rs, std::move(steps));
	load(adapter, k_model, bytes());
	BOOST_REQUIRE(rs->exists(k_geom));
	const warnings warned;
	load(adapter, k_model, text_bytes("o n"));
	BOOST_TEST(imports == 2);
	BOOST_TEST(!geom_there_at_import);
	BOOST_TEST(warned.count("cannot list") == 1u);
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
		return edt::decoded_model_adapter(*descs, *rs, edt::decoded_model_adapter::steps_of(std::make_shared<edt::process_decoder>(*rs, *descs, config)));
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
	const auto loaded = load(adapter(), res::tag("res://ram/gen_robot.glb"), robot);
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
BOOST_AUTO_TEST_CASE(TheFileSourceOfAModelGivenOnlyAsBytes_ServesTheModelAndRefusesEveryOtherFile)
{
	dir.write("ram/m.mtl", "newmtl red\n");
	const fs::path model = dir.path() / "ram" / "m.obj";
	const std::vector<std::byte> bytes = text("v 0 0 0\n");
	edt::model_file_access access(model, {}, nullptr, {}, false);
	edt::model_file_source files(model, bytes, &access);

	BOOST_TEST(files.exists(model.string()));
	const mdl::opened_file opened = files.open(model.string());
	BOOST_REQUIRE(opened.bytes != nullptr);
	BOOST_TEST(opened.bytes->size() == bytes.size());

	const std::string side = (dir.path() / "ram" / "m.mtl").string();
	for (const std::string& name : { std::string("m.mtl"), side }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_CHECK_THROW(files.exists(name), std::runtime_error);
			BOOST_CHECK_THROW(files.open(name), std::runtime_error);
		}
	}
	BOOST_TEST(access.files().empty());
}

BOOST_AUTO_TEST_SUITE_END()

#endif
