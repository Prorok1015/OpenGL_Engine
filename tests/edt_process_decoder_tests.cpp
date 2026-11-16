#include <boost/test/unit_test.hpp>
#include "edt_process_decoder.h"
#include "test_decoder_sandbox.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <thread>
#include <utility>
#include <vector>

// What the editor accepts of the decoder process's answer (#647 PR 2): checked before anything is stored, and what the
// process-backed import does with a decoder that is missing, slow, cancelled or lied to.

namespace {

namespace fs = std::filesystem;

const fs::path k_models = fs::temp_directory_path() / "snk_process_decoder_models";

edt::decode_request request_in(const fs::path& model = k_models / "m.obj")
{
	edt::decode_request request;
	request.source_path = model;
	return request;
}

const res::tag k_prefab("memory://m/m.prefab.desc");
const res::tag k_geom("memory://m/m.geom.desc");
const res::tag k_material("memory://m/materials/x.mat.desc");
const res::tag k_texture("memory://m/textures/t.png");
const res::tag k_texture_desc("memory://m/textures/t.png.txm.desc");

std::vector<std::byte> text_bytes(std::string_view text)
{
	std::vector<std::byte> bytes(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = static_cast<std::byte>(text[i]);
	return bytes;
}

std::vector<std::byte> png_bytes() { return { std::byte{ 0x89 }, std::byte{ 0x50 }, std::byte{ 0x4E }, std::byte{ 0x47 }, std::byte{ 0x0D }, std::byte{ 0x0A }, std::byte{ 0x1A }, std::byte{ 0x0A }, std::byte{ 0 } }; }

// A string as JSON text between quotes: a backslash doubled.
std::string edt_json_escaped(std::string_view text)
{
	std::string out;
	for (const char c : text) {
		if (c == '\\') out += "\\\\";
		else out.push_back(c);
	}
	return out;
}

edt::bundle_blob blob_of(const res::tag& tag, std::vector<std::byte> bytes) { return { std::string(tag.view()), std::move(bytes) }; }

// The answer a decoder gives for m.obj: a prefab, a geometry, a material and a texture with its desc.
edt::decode_bundle import_answer(boost::json::object prefab_json = {})
{
	edt::import_intermediate imported;
	imported.prefab_tag = k_prefab;
	imported.geom_tag = k_geom;
	imported.created_tags = { k_prefab, k_geom, k_material, k_texture, k_texture_desc };
	imported.all_tags = imported.created_tags;
	imported.source_path = "somewhere/else.obj";
	imported.prefab_json = std::move(prefab_json);
	edt::decode_bundle bundle;
	bundle.manifest["kind"] = "import";
	bundle.manifest["import"] = edt::to_json(imported);
	bundle.blobs.push_back(blob_of(k_prefab, text_bytes(R"json({"__type":"prefab_desc","geometry":"memory://m/m.geom.desc"})json")));
	bundle.blobs.push_back(blob_of(k_geom, text_bytes(R"json({"__type":"geometry_desc","__parent":"res://base_geometry.desc"})json")));
	bundle.blobs.push_back(blob_of(k_material, text_bytes(R"json({"__type":"material_desc","__parent":"res://base_material.desc","name":"x","shader_fragment":"res://shaders/mix_opaque_trans_scene.frag"})json")));
	bundle.blobs.push_back(blob_of(k_texture, png_bytes()));
	bundle.blobs.push_back(blob_of(k_texture_desc, text_bytes(R"json({"__type":"texture_2d_desc","__parent":"res://base_texture.desc","data":"memory://m/textures/t.png"})json")));
	return bundle;
}

// The answer rebuilt with other tags, resources and fields, for the hostile cases.
edt::decode_bundle answer_with(const std::vector<edt::bundle_blob>& blobs, const std::vector<res::tag>& listed, const std::vector<res::tag>& created)
{
	edt::import_intermediate imported;
	imported.prefab_tag = k_prefab;
	imported.geom_tag = k_geom;
	imported.created_tags = created;
	imported.all_tags = listed;
	edt::decode_bundle bundle;
	bundle.manifest["kind"] = "import";
	bundle.manifest["import"] = edt::to_json(imported);
	bundle.blobs = blobs;
	return bundle;
}

edt::decode_bundle scan_answer(const std::vector<edt::requested_file>& requested)
{
	edt::scan_requests requests;
	requests.requested = requested;
	edt::decode_bundle bundle;
	bundle.manifest = edt::to_json(requests);
	bundle.manifest["kind"] = "scan";
	return bundle;
}

}

BOOST_AUTO_TEST_SUITE(EdtAcceptDecoderAnswerTests)

BOOST_AUTO_TEST_CASE(AnImportAnswer_ThatMatchesWhatWasAsked_IsAccepted_AndTakesTheModelPathFromTheRequest)
{
	const auto request = request_in();
	std::string problem;
	const auto imported = edt::accept_import_answer(import_answer(), request, problem);
	BOOST_REQUIRE_MESSAGE(imported.has_value(), problem);
	BOOST_CHECK(imported->error.empty());
	BOOST_CHECK_EQUAL(imported->created_tags.size(), 5u);
	BOOST_CHECK(imported->source_path == request.source_path);
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_OfAnotherKind_OrWithoutAnImport_IsRefused)
{
	const auto request = request_in();
	std::string problem;
	auto bundle = import_answer();
	bundle.manifest["kind"] = "scan";
	BOOST_CHECK(!edt::accept_import_answer(bundle, request, problem).has_value());

	bundle = import_answer();
	bundle.manifest.erase("import");
	BOOST_CHECK(!edt::accept_import_answer(bundle, request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_WithAResourceItDidNotCreate_OrWithoutOneItDid_IsRefused)
{
	const auto request = request_in();
	std::string problem;

	auto extra = import_answer();
	extra.blobs.push_back({ "memory://m/textures/stray.png", png_bytes() });
	BOOST_CHECK(!edt::accept_import_answer(extra, request, problem).has_value());

	auto missing = import_answer();
	missing.blobs.pop_back();
	BOOST_CHECK(!edt::accept_import_answer(missing, request, problem).has_value());

	auto renamed = import_answer();
	renamed.blobs.back().tag = "memory://m/textures/other.png.txm.desc";
	BOOST_CHECK(!edt::accept_import_answer(renamed, request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_WithAModelFileAsAResource_IsRefused_WhateverItsFolder)
{
	const auto request = request_in();
	std::string problem;
	// The exporter would copy it into the project, and loading what refers to it would parse it inside the editor.
	for (const char* name : { "memory://m/evil.fbx", "memory://m/textures/evil.glb", "memory://m/textures/evil.gltf", "memory://m/textures/evil.obj",
		"memory://m/__embedded_txm/evil.fbx", "memory://m/materials/evil.fbx" }) {
		const std::vector<res::tag> tags = { k_prefab, k_geom, res::tag(name) };
		const std::vector<edt::bundle_blob> blobs = { blob_of(k_prefab, text_bytes("{}")), blob_of(k_geom, text_bytes("{}")), { name, text_bytes("raw model bytes") } };
		BOOST_CHECK_MESSAGE(!edt::accept_import_answer(answer_with(blobs, tags, tags), request, problem).has_value(), name);
	}
}

BOOST_AUTO_TEST_CASE(AnImageThatIsNotAnImage_IsRefused_AndOneUnderADoubleExtensionIsStillAnImage)
{
	const auto request = request_in();
	std::string problem;
	auto not_an_image = import_answer();
	not_an_image.blobs[3].bytes = text_bytes("this is no png");
	BOOST_CHECK(!edt::accept_import_answer(not_an_image, request, problem).has_value());
	BOOST_CHECK_MESSAGE(problem.find("not what its name says") != std::string::npos, problem);

	// A real PNG under "evil.fbx.png" stays an image: the adapter takes the last extension.
	const res::tag double_name("memory://m/textures/evil.fbx.png");
	const std::vector<res::tag> tags = { k_prefab, k_geom, double_name };
	const std::vector<edt::bundle_blob> blobs = { blob_of(k_prefab, text_bytes("{}")), blob_of(k_geom, text_bytes("{}")), blob_of(double_name, png_bytes()) };
	const auto accepted = edt::accept_import_answer(answer_with(blobs, tags, tags), request, problem);
	BOOST_CHECK_MESSAGE(accepted.has_value(), problem);
}

BOOST_AUTO_TEST_CASE(ADescThatPointsOutsideTheImport_IsRefused)
{
	const auto request = request_in();
	std::string problem;
	for (const char* reference : { "edt://settings/agent_policy.json", "user://x.desc", "cache://x", "engine://x", "code://x.cpp",
		"res://some/file.desc", "memory://modules/other.desc", "memory://m/not_in_the_bundle.desc", "user/x.desc" }) {
		auto bundle = import_answer();
		bundle.blobs[4].bytes = text_bytes(std::string(R"json({"__type":"texture_2d_desc","__parent":"res://base_texture.desc","data":")json") + reference + "\"}");
		BOOST_CHECK_MESSAGE(!edt::accept_import_answer(bundle, request, problem).has_value(), reference);
	}
}

BOOST_AUTO_TEST_CASE(ADescWithAParentThatIsNotABaseResource_OrAForbiddenKey_OrBadJson_IsRefused)
{
	const auto request = request_in();
	std::string problem;
	const auto refused = [&](const char* text) {
		auto bundle = import_answer();
		bundle.blobs[2].bytes = text_bytes(text);
		return !edt::accept_import_answer(bundle, request, problem).has_value();
	};
	BOOST_CHECK(refused(R"json({"__type":"material_desc","__parent":"memory://m/m.geom.desc"})json"));
	BOOST_CHECK(refused(R"json({"__type":"material_desc","__parent":"edt://settings/x"})json"));
	BOOST_CHECK(refused(R"json({"__type":"material_desc","__parent":["res://base_material.desc"]})json"));
	BOOST_CHECK(refused(R"json({"__type":"material_desc","__secret":1})json"));
	BOOST_CHECK(refused("not json"));
	BOOST_CHECK(refused("[1,2]"));
}

// #772: the ID of an imported file is the .resource the editor gives it; an importer that writes one is refused, at any depth.
BOOST_AUTO_TEST_CASE(ADescOfAnImportWithAGuid_IsRefused)
{
	const auto request = request_in();
	std::string problem;
	const auto refused = [&](const char* text) {
		auto bundle = import_answer();
		bundle.blobs[2].bytes = text_bytes(text);
		return !edt::accept_import_answer(bundle, request, problem).has_value();
	};
	BOOST_CHECK(refused(R"json({"__type":"material_desc","__guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"})json"));
	BOOST_TEST(problem.find("__guid") != std::string::npos);
	BOOST_TEST_MESSAGE(problem);
	BOOST_TEST(problem.starts_with("'"));
	BOOST_TEST(problem.find(".desc': a desc of the import has the key '__guid'") != std::string::npos);
	BOOST_CHECK(refused(R"json({"__type":"material_desc","inner":{"__type":"texture_desc","__guid":"x"}})json"));
}

BOOST_AUTO_TEST_CASE(ThePrefabJsonOfAnImport_IsHeldToTheSameRules)
{
	const auto request = request_in();
	std::string problem;
	boost::json::object bad;
	bad["geometry"] = "user://x.geom.desc";
	BOOST_CHECK(!edt::accept_import_answer(import_answer(bad), request, problem).has_value());

	boost::json::object good;
	good["geometry"] = "memory://m/m.geom.desc";
	good["name"] = "a plain name";
	BOOST_CHECK(edt::accept_import_answer(import_answer(good), request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(TwoTagsThatDifferOnlyByCase_AreOneFileOnWindows_AndRefusedAsARepeat)
{
	const auto request = request_in();
	std::string problem;
	const res::tag first("memory://m/textures/T.png");
	const res::tag second("memory://m/textures/t.PNG");
	const std::vector<res::tag> tags = { k_prefab, k_geom, first, second };
	const std::vector<edt::bundle_blob> blobs = { blob_of(k_prefab, text_bytes("{}")), blob_of(k_geom, text_bytes("{}")), blob_of(first, png_bytes()), blob_of(second, png_bytes()) };
	BOOST_CHECK(!edt::accept_import_answer(answer_with(blobs, tags, tags), request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(ADescStringWithBackslashes_IsReadAsATagTheWayTheResourceSystemReadsIt)
{
	const auto request = request_in();
	std::string problem;
	// res::tag turns a backslash into a slash first: "res\\x\\y.fbx" is res://x/y.fbx, "edt\\x" is edt://x.
	for (const char* reference : { "res\\\\robot\\\\robot.fbx", "res\\robot\\robot.fbx", "edt\\settings\\agent_policy.json", "user\\x.desc", "cache\\x", "code\\x.cpp", "engine\\x" }) {
		auto bundle = import_answer();
		bundle.blobs[4].bytes = text_bytes(std::string(R"json({"__type":"texture_2d_desc","__parent":"res://base_texture.desc","data":")json") + edt_json_escaped(reference) + "\"}");
		BOOST_CHECK_MESSAGE(!edt::accept_import_answer(bundle, request, problem).has_value(), reference);
	}
	// A Windows path in a name is not a tag: the first segment is no protocol.
	auto plain = import_answer();
	plain.blobs[4].bytes = text_bytes(std::string(R"json({"__type":"texture_2d_desc","__parent":"res://base_texture.desc","data":"memory://m/textures/t.png","name":"textures\\a.png"})json"));
	BOOST_CHECK_MESSAGE(edt::accept_import_answer(plain, request, problem).has_value(), problem);
}

BOOST_AUTO_TEST_CASE(AResourceNameThatCannotBeAFileOnWindows_IsRefused)
{
	const auto request = request_in();
	std::string problem;
	for (const char* name : { "memory://m/textures/con.png", "memory://m/textures/NUL.png", "memory://m/textures/com1.png", "memory://m/textures/a.png.",
		"memory://m/textures/a .png.txm.desc ", "memory://m/textures/a?.png", "memory://m/textures/a<b.png" }) {
		const std::vector<res::tag> tags = { k_prefab, k_geom, res::tag(name) };
		const std::vector<edt::bundle_blob> blobs = { blob_of(k_prefab, text_bytes("{}")), blob_of(k_geom, text_bytes("{}")), { name, png_bytes() } };
		BOOST_CHECK_MESSAGE(!edt::accept_import_answer(answer_with(blobs, tags, tags), request, problem).has_value(), name);
	}
}

BOOST_AUTO_TEST_CASE(ADescWithAKeyThatReadsAsATag_IsRefused)
{
	const auto request = request_in();
	std::string problem;
	auto bundle = import_answer();
	bundle.blobs[2].bytes = text_bytes(R"json({"__type":"material_desc","__parent":"res://base_material.desc","user://x":1})json");
	BOOST_CHECK(!edt::accept_import_answer(bundle, request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_ThatListsATagItDidNotCreate_OrPutsTheFolderElsewhere_IsRefused)
{
	const auto request = request_in();
	std::string problem;

	// Listed but not created: the exporter walks the listed tags, so someone else's memory:// would be copied.
	const std::vector<edt::bundle_blob> blobs = { blob_of(k_prefab, text_bytes("{}")), blob_of(k_geom, text_bytes("{}")) };
	auto listed_only = answer_with(blobs, { k_prefab, k_geom, k_material }, { k_prefab, k_geom });
	BOOST_CHECK(!edt::accept_import_answer(listed_only, request, problem).has_value());

	auto twice = answer_with(blobs, { k_prefab, k_geom }, { k_prefab, k_geom, k_geom });
	BOOST_CHECK(!edt::accept_import_answer(twice, request, problem).has_value());

	// The folder is the stem of the model the editor asked about, not what the child says.
	const auto other = request_in(k_models / "other.obj");
	BOOST_CHECK(!edt::accept_import_answer(import_answer(), other, problem).has_value());
	BOOST_CHECK(edt::accept_import_answer(answer_with(blobs, { k_prefab, k_geom }, { k_prefab, k_geom }), request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_BiggerThanTheRequestAllows_IsRefused)
{
	auto request = request_in();
	request.limits.max_file_bytes = 4;
	request.limits.max_total_bytes = 1000;
	std::string problem;
	BOOST_CHECK(!edt::accept_import_answer(import_answer(), request, problem).has_value());
	BOOST_CHECK_MESSAGE(problem.find("bigger than the request") != std::string::npos, problem);

	request = request_in();
	request.limits.max_total_bytes = 10;
	BOOST_CHECK(!edt::accept_import_answer(import_answer(), request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AFailedImport_IsHandedBackAsItsError_ButNeverWithResources)
{
	const auto request = request_in();
	std::string problem;
	edt::decode_bundle failed;
	failed.manifest["kind"] = "import";
	failed.manifest["error"] = "the model refers to\n'x': not allowed";
	const auto imported = edt::accept_import_answer(failed, request, problem);
	BOOST_REQUIRE(imported.has_value());
	BOOST_CHECK(!imported->error.empty());
	BOOST_CHECK_EQUAL(imported->error.find('\n'), std::string::npos);
	BOOST_CHECK(imported->created_tags.empty());

	failed.blobs.push_back({ "memory://m/x.desc", { std::byte{ 1 } } });
	BOOST_CHECK(!edt::accept_import_answer(failed, request, problem).has_value());
}

// #1043: the scan names files, the editor's rule decides whether they are read; the answer itself only has to be one.
BOOST_AUTO_TEST_CASE(AScanAnswer_ThatAsksForFiles_IsAccepted_WhateverTheNamesPointAt)
{
	std::string problem;
	const auto good = edt::accept_scan_answer(scan_answer({ { "m.mtl", edt::model_file_kind::side_file },
		{ "../../secret.png", edt::model_file_kind::texture } }), problem);
	BOOST_REQUIRE_MESSAGE(good.has_value(), problem);
	BOOST_REQUIRE_EQUAL(good->requested.size(), 2u);
	BOOST_CHECK(good->requested[0].kind == edt::model_file_kind::side_file);
	BOOST_CHECK(good->error.empty());

	edt::decode_bundle none;
	none.manifest["kind"] = "scan";
	BOOST_CHECK(!edt::accept_scan_answer(none, problem).has_value());
	auto other = scan_answer({});
	other.manifest["kind"] = "import";
	BOOST_CHECK(!edt::accept_scan_answer(other, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_ForAModelGivenOnlyAsBytes_CarriesNoImageFile)
{
	// The answer carries textures/t.png, an image file the model named: an honest import from bytes cannot have read one.
	auto request = request_in();
	request.on_disk = false;
	std::string problem;
	BOOST_CHECK(!edt::accept_import_answer(import_answer(), request, problem).has_value());
	BOOST_CHECK_MESSAGE(problem.find("not a file on the disk carries the image file 'textures/t.png'") != std::string::npos, problem);
}

BOOST_AUTO_TEST_CASE(AnImportAnswer_GoesToTheMemoryFolderTheRequestNamed_AndNowhereElse)
{
	auto request = request_in();
	request.memory_folder = "__models/res/a/m/";
	const res::tag prefab("memory://__models/res/a/m/m.prefab.desc");
	const res::tag geom("memory://__models/res/a/m/m.geom.desc");
	edt::import_intermediate imported;
	imported.prefab_tag = prefab;
	imported.geom_tag = geom;
	imported.created_tags = { prefab, geom };
	imported.all_tags = imported.created_tags;
	edt::decode_bundle bundle;
	bundle.manifest["kind"] = "import";
	bundle.manifest["import"] = edt::to_json(imported);
	bundle.blobs.push_back(blob_of(prefab, text_bytes(R"json({"__type":"prefab_desc","geometry":"memory://__models/res/a/m/m.geom.desc"})json")));
	bundle.blobs.push_back(blob_of(geom, text_bytes(R"json({"__type":"geometry_desc","__parent":"res://base_geometry.desc"})json")));
	std::string problem;
	BOOST_REQUIRE_MESSAGE(edt::accept_import_answer(bundle, request, problem).has_value(), problem);

	// The folder of the model's stem is not where this request puts the import.
	BOOST_CHECK(!edt::accept_import_answer(import_answer(), request, problem).has_value());

	for (const char* folder : { "__models/res/a/m", "__models//m/", "__models/../m/", "__models/con/m/" }) {
		BOOST_TEST_CONTEXT(folder) {
			request.memory_folder = folder;
			problem.clear();
			BOOST_CHECK(!edt::accept_import_answer(bundle, request, problem).has_value());
			BOOST_CHECK_MESSAGE(problem.find("cannot be the folder of an import") != std::string::npos, problem);
		}
	}
}

BOOST_AUTO_TEST_CASE(AScanAnswer_ThatAsksForTooManyFiles_OrComesWithResources_IsRefused)
{
	std::string problem;
	std::vector<edt::requested_file> many;
	for (int i = 0; i < 256; ++i) many.push_back({ "t" + std::to_string(i) + ".png", edt::model_file_kind::texture });
	BOOST_CHECK_MESSAGE(edt::accept_scan_answer(scan_answer(many), problem).has_value(), problem);
	many.push_back({ "one_more.png", edt::model_file_kind::texture });
	BOOST_CHECK(!edt::accept_scan_answer(scan_answer(many), problem).has_value());
	BOOST_CHECK_MESSAGE(problem.find("more files than it may") != std::string::npos, problem);

	auto with_blob = scan_answer({});
	with_blob.blobs.push_back({ "memory://m/x.desc", { std::byte{ 1 } } });
	BOOST_CHECK(!edt::accept_scan_answer(with_blob, problem).has_value());
	BOOST_CHECK_MESSAGE(problem.find("came with resources") != std::string::npos, problem);
}

BOOST_AUTO_TEST_CASE(AScanAnswer_ThatAsksByANameThatCannotBeOne_IsRefused)
{
	for (const std::string& name : { std::string(), std::string("a\nb.mtl"), std::string("a\x1b[2Jb.png"), std::string("a\0b.png", 7),
		std::string(1025, 'a') }) {
		BOOST_TEST_CONTEXT(name.size()) {
			std::string problem;
			BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ { name, edt::model_file_kind::side_file } }), problem).has_value());
			BOOST_CHECK_MESSAGE(problem.find("by a name that cannot be one") != std::string::npos, problem);
		}
	}
	std::string problem;
	BOOST_CHECK(edt::accept_scan_answer(scan_answer({ { std::string(1024, 'a'), edt::model_file_kind::side_file } }), problem).has_value());

	auto bad_kind = scan_answer({ { "m.mtl", edt::model_file_kind::side_file } });
	bad_kind.manifest["requests"].as_array()[0].as_object()["kind"] = "model";
	BOOST_CHECK(!edt::accept_scan_answer(bad_kind, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AScanThatTheChildReportedAsFailed_ComesBackWithItsError)
{
	edt::decode_bundle failed;
	failed.manifest["kind"] = "scan";
	failed.manifest["error"] = "std::bad_alloc";
	std::string problem;
	const auto scan = edt::accept_scan_answer(failed, problem);
	BOOST_REQUIRE(scan.has_value());
	BOOST_CHECK(scan->requested.empty());
	BOOST_CHECK_EQUAL(scan->error, "the decoder ran out of memory");

	failed.manifest["error"] = "the model cannot be read:\nnope";
	const auto cleaned = edt::accept_scan_answer(failed, problem);
	BOOST_REQUIRE(cleaned.has_value());
	BOOST_CHECK_EQUAL(cleaned->error.find('\n'), std::string::npos);
}

// #1043: the child says how it is sandboxed in its first stderr line, before it reads the request; a later line may be
// the model's, which must not be able to say the sandbox is on.
BOOST_AUTO_TEST_CASE(OnlyTheChildsFirstStderrLine_SaysHowItIsSandboxed)
{
	edt::sandbox_status_reader reader;
	BOOST_CHECK(reader.line("snk-sandbox-status: fs=none syscalls=seccomp why=no Landlock"));
	BOOST_CHECK(reader.status().reported);
	BOOST_CHECK(!reader.status().fs);
	BOOST_CHECK(reader.status().syscalls);
	BOOST_CHECK_EQUAL(reader.status().why, "no Landlock");
	BOOST_CHECK(!reader.line("snk-sandbox-status: fs=landlock syscalls=seccomp"));
	BOOST_CHECK(!reader.status().fs);

	edt::sandbox_status_reader full;
	BOOST_CHECK(full.line("snk-sandbox-status: fs=landlock syscalls=seccomp"));
	BOOST_CHECK(full.status().reported && full.status().fs && full.status().syscalls);
	BOOST_CHECK(full.status().why.empty());

	edt::sandbox_status_reader late;
	BOOST_CHECK(!late.line("some parser warning"));
	BOOST_CHECK(!late.line("snk-sandbox-status: fs=landlock syscalls=seccomp"));
	BOOST_CHECK(!late.status().reported);

	for (const char* garbled : { "snk-sandbox-status: fs=maybe syscalls=seccomp", "snk-sandbox-status: fs=landlock syscalls=seccomp more",
			 "snk-sandbox-status: fs=landlock" }) {
		edt::sandbox_status_reader reading;
		BOOST_CHECK(reading.line(garbled));
		BOOST_CHECK_MESSAGE(!reading.status().reported, garbled);
	}
}

BOOST_AUTO_TEST_CASE(TheSandboxMode_IsThePersonsSetting_RaisedNeverLoweredByTheImport_AndAnUnknownCfgValueIsOn)
{
	using edt::sandbox_mode;
	// What an import asks for raises the person's setting, never lowers it.
	BOOST_CHECK(edt::effective_sandbox_mode(sandbox_mode::best_effort, sandbox_mode::off) == sandbox_mode::best_effort);
	BOOST_CHECK(edt::effective_sandbox_mode(sandbox_mode::off, sandbox_mode::off) == sandbox_mode::off);
	BOOST_CHECK(edt::effective_sandbox_mode(sandbox_mode::off, sandbox_mode::required) == sandbox_mode::required);
	BOOST_CHECK(edt::effective_sandbox_mode(sandbox_mode::best_effort, sandbox_mode::required) == sandbox_mode::required);
	BOOST_CHECK(edt::effective_sandbox_mode(sandbox_mode::required, sandbox_mode::off) == sandbox_mode::required);

	std::string warning;
	BOOST_CHECK(edt::decoder_sandbox_mode("on", warning) == sandbox_mode::best_effort);
	BOOST_CHECK(warning.empty());
	BOOST_CHECK(edt::decoder_sandbox_mode("off", warning) == sandbox_mode::off);
	BOOST_CHECK(warning.empty());
	BOOST_CHECK(edt::decoder_sandbox_mode("required", warning) == sandbox_mode::required);
	BOOST_CHECK(warning.empty());
	BOOST_CHECK(edt::decoder_sandbox_mode("OFF", warning) == sandbox_mode::best_effort);
	BOOST_CHECK(warning.find("import/decoder_sandbox") != std::string::npos);
	BOOST_CHECK(!warning.empty());
}

BOOST_AUTO_TEST_CASE(TheDecoderMemory_DefaultsTo2GiB_AndIsClampedWithAWarning_NeverUnlimited)
{
	constexpr std::uint64_t mib = 1024ull * 1024ull;
	std::string warning;
	BOOST_CHECK_EQUAL(edt::decoder_memory_limit(0, warning), 2048 * mib);
	BOOST_CHECK(warning.empty());
	BOOST_CHECK_EQUAL(edt::decoder_memory_limit(-5, warning), 2048 * mib);
	BOOST_CHECK_EQUAL(edt::decoder_memory_limit(1024, warning), 1024 * mib);
	BOOST_CHECK(warning.empty());
	BOOST_CHECK_EQUAL(edt::decoder_memory_limit(10, warning), 256 * mib);
	BOOST_CHECK(!warning.empty());
	BOOST_CHECK_EQUAL(edt::decoder_memory_limit(1'000'000, warning), 8192 * mib);
	BOOST_CHECK(!warning.empty());
}

BOOST_AUTO_TEST_CASE(WhatTheChildPrints_HasNoControlCharacterBidiMarkOrLineSeparatorLeft)
{
	BOOST_CHECK_EQUAL(edt::decode_log_text("plain text\tand a tab"), "plain text\tand a tab");
	BOOST_CHECK_EQUAL(edt::decode_log_text("a\nb\x1b[31mc\x7f"), "a\\x0ab\\x1b[31mc\\x7f");
	// U+0085 (a C1 control), U+202E (right-to-left override), U+2028 and U+2067 (an isolate), each as its UTF-8 bytes.
	BOOST_CHECK_EQUAL(edt::decode_log_text("x\xC2\x85y"), "x\\xc2\\x85y");
	BOOST_CHECK_EQUAL(edt::decode_log_text("x\xE2\x80\xAEy"), "x\\xe2\\x80\\xaey");
	BOOST_CHECK_EQUAL(edt::decode_log_text("x\xE2\x80\xA8y"), "x\\xe2\\x80\\xa8y");
	BOOST_CHECK_EQUAL(edt::decode_log_text("x\xE2\x81\xA7y"), "x\\xe2\\x81\\xa7y");
	// Letters of other scripts and a cut sequence are not touched.
	BOOST_CHECK_EQUAL(edt::decode_log_text("\xD0\x9F\xD1\x80"), "\xD0\x9F\xD1\x80");
	BOOST_CHECK_EQUAL(edt::decode_log_text("x\xE2\x80"), "x\xE2\x80");
}

BOOST_AUTO_TEST_CASE(TheLogOfARun_IsCutAfterTwoHundredLinesOrSixtyFourKiB_Once)
{
	edt::decode_log_budget lines;
	int taken = 0;
	for (int i = 0; i < 1000; ++i) taken += lines.take(10) ? 1 : 0;
	BOOST_CHECK_EQUAL(taken, 200);

	edt::decode_log_budget bytes;
	BOOST_CHECK(bytes.take(40 * 1024));
	BOOST_CHECK(!bytes.take(40 * 1024));
	BOOST_CHECK(!bytes.take(1));
}


// ---- the geometry pair of an answer (#1068 PR3b): decoded as the loader decodes it, encoded again by the editor ----

namespace {

const res::tag k_geom_bin("memory://m/m.geom.bin");

rnd::geometry_desc one_triangle()
{
	rnd::geometry_desc out;
	out.layout = rnd::driver::BufferLayout{ { rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" } };
	const float floats[] = { -1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f };
	out.vertices.resize(sizeof(floats));
	std::memcpy(out.vertices.data(), floats, sizeof(floats));
	out.indices = { 0, 1, 2 };
	return out;
}

// What the editor's writer makes of one triangle: the pair a decoder sends back.
rnd::geometry_files triangle_pair()
{
	rnd::geometry_file_options options;
	options.header["__parent"] = "res://base_geometry.desc";
	rnd::geometry_files files = rnd::encode_geometry_files(k_geom, one_triangle(), options);
	BOOST_REQUIRE_MESSAGE(files, files.error);
	return files;
}

boost::json::object json_of(const std::vector<std::byte>& bytes)
{
	return boost::json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).as_object();
}

std::vector<std::byte> bytes_of_json(const boost::json::object& object)
{
	return text_bytes(boost::json::serialize(object));
}

// The answer of import_answer() with the geometry as `desc` and, when given, the binary file `binary` (created, not listed).
edt::decode_bundle answer_with_geometry(const std::vector<std::byte>& desc, const std::optional<std::vector<std::byte>>& binary,
	bool list_binary = false)
{
	edt::decode_bundle bundle = import_answer();
	for (edt::bundle_blob& blob : bundle.blobs) {
		if (blob.tag == k_geom.view()) blob.bytes = desc;
	}
	std::vector<res::tag> created = { k_prefab, k_geom, k_material, k_texture, k_texture_desc };
	std::vector<res::tag> listed = created;
	if (binary) {
		bundle.blobs.push_back(blob_of(k_geom_bin, *binary));
		created.push_back(k_geom_bin);
		if (list_binary) listed.push_back(k_geom_bin);
	}
	return answer_with(bundle.blobs, listed, created);
}

// The range `key` of `desc` over `binary` patched by `patch`, its hash computed again: what a decoder that lies about the data
// but not about the hash sends.
void patch_range(boost::json::object& desc, std::vector<std::byte>& binary, std::string_view key,
	const std::function<void(std::span<std::byte>)>& patch)
{
	boost::json::object& range = desc.at(key).as_object();
	const std::size_t offset = static_cast<std::size_t>(range.at("offset").to_number<std::uint64_t>());
	const std::size_t size = static_cast<std::size_t>(range.at("size").to_number<std::uint64_t>());
	const std::span<std::byte> bytes(binary.data() + offset, size);
	patch(bytes);
	range["sha256"] = cmn::sha256_hex(bytes);
}

bool refused(const edt::decode_bundle& bundle, std::string_view expected)
{
	std::string problem;
	const bool accepted = edt::accept_import_answer(bundle, request_in(), problem).has_value();
	BOOST_TEST_MESSAGE("problem: " << problem);
	return !accepted && problem.find(expected) != std::string::npos;
}

}

BOOST_AUTO_TEST_CASE(AGeometryPair_IsAccepted_AndStoredAsTheEditorEncodesIt)
{
	const rnd::geometry_files pair = triangle_pair();
	const auto bundle = answer_with_geometry(pair.desc, pair.binary);
	std::string problem;
	rnd::geometry_files rewritten;
	const auto imported = edt::accept_import_answer(bundle, request_in(), problem, &rewritten);
	BOOST_REQUIRE_MESSAGE(imported.has_value(), problem);
	BOOST_REQUIRE_MESSAGE(rewritten, rewritten.error);
	// The editor's writer over the decoded data: the same bytes it writes for the same triangle.
	BOOST_TEST((rewritten.desc == pair.desc));
	BOOST_TEST((rewritten.binary == pair.binary));

	// Without the out parameter (the cache path) the answer is only checked.
	BOOST_TEST(edt::accept_import_answer(bundle, request_in(), problem).has_value());
}

BOOST_AUTO_TEST_CASE(ABinaryFileForADescWithoutRanges_IsRefused)
{
	const rnd::geometry_files pair = triangle_pair();
	const std::vector<std::byte> no_data = text_bytes(R"json({"__type":"geometry_desc","__parent":"res://base_geometry.desc"})json");
	BOOST_TEST(refused(answer_with_geometry(no_data, pair.binary), "does not refer to one"));
}

BOOST_AUTO_TEST_CASE(ADescWithRanges_WithoutItsBinaryFile_IsRefused)
{
	const rnd::geometry_files pair = triangle_pair();
	BOOST_TEST(refused(answer_with_geometry(pair.desc, std::nullopt), "did not come with the answer"));
}

BOOST_AUTO_TEST_CASE(ABinaryFile_ListedOrUnderAnotherName_OrTwice_IsRefused)
{
	const rnd::geometry_files pair = triangle_pair();
	BOOST_TEST(refused(answer_with_geometry(pair.desc, pair.binary, true), "lists the binary file"));

	for (const char* name : { "memory://m/other.geom.bin", "memory://m/textures/m.geom.bin", "memory://m/M.geom.bin" }) {
		auto bundle = answer_with_geometry(pair.desc, pair.binary);
		auto imported = edt::import_intermediate_from_json(bundle.manifest.at("import"));
		BOOST_REQUIRE(imported.has_value());
		imported->created_tags.push_back(res::tag(name));
		bundle.manifest["import"] = edt::to_json(*imported);
		bundle.blobs.push_back(blob_of(res::tag(name), pair.binary));
		BOOST_TEST_INFO(name);
		std::string problem;
		BOOST_TEST(!edt::accept_import_answer(bundle, request_in(), problem).has_value());
	}
}

BOOST_AUTO_TEST_CASE(OverlappingRanges_ARangePastTheFile_AndAHugeRangeSize_AreRefused)
{
	const rnd::geometry_files pair = triangle_pair();

	boost::json::object overlapping = json_of(pair.desc);
	overlapping["indices"].as_object()["offset"] = overlapping["vertices"].as_object()["offset"];
	BOOST_TEST(refused(answer_with_geometry(bytes_of_json(overlapping), pair.binary), "the geometry of the import is refused"));

	boost::json::object past = json_of(pair.desc);
	past["indices"].as_object()["size"] = pair.binary.size();
	BOOST_TEST(refused(answer_with_geometry(bytes_of_json(past), pair.binary), "the geometry of the import is refused"));

	boost::json::object huge = json_of(pair.desc);
	huge["vertices"].as_object()["size"] = std::uint64_t{ 0xFFFFFFFFFFFFFFF0ull };
	BOOST_TEST(refused(answer_with_geometry(bytes_of_json(huge), pair.binary), "the geometry of the import is refused"));
}

BOOST_AUTO_TEST_CASE(ABinaryFile_WithAnotherHashOrHeader_OrCut_IsRefused)
{
	const rnd::geometry_files pair = triangle_pair();

	std::vector<std::byte> changed = pair.binary;
	changed.back() ^= std::byte{ 1 };
	BOOST_TEST(refused(answer_with_geometry(pair.desc, changed), "the geometry of the import is refused"));

	std::vector<std::byte> header = pair.binary;
	header[0] = std::byte{ 'X' };
	BOOST_TEST(refused(answer_with_geometry(pair.desc, header), "the geometry of the import is refused"));

	std::vector<std::byte> cut = pair.binary;
	cut.resize(cut.size() / 2);
	BOOST_TEST(refused(answer_with_geometry(pair.desc, cut), "the geometry of the import is refused"));
}

// The hashes match the lie: the loader's data checks refuse it all the same.
BOOST_AUTO_TEST_CASE(AnIndexPastTheVertices_OrAPositionThatIsNotFinite_IsRefused_WithTheirHashesRight)
{
	const rnd::geometry_files pair = triangle_pair();

	boost::json::object desc = json_of(pair.desc);
	std::vector<std::byte> binary = pair.binary;
	patch_range(desc, binary, "indices", [](std::span<std::byte> bytes) {
		const std::uint32_t past = 7;
		std::memcpy(bytes.data(), &past, sizeof(past));
	});
	BOOST_TEST(refused(answer_with_geometry(bytes_of_json(desc), binary), "past the"));

	desc = json_of(pair.desc);
	binary = pair.binary;
	patch_range(desc, binary, "vertices", [](std::span<std::byte> bytes) {
		const float nan = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(bytes.data(), &nan, sizeof(nan));
	});
	BOOST_TEST(refused(answer_with_geometry(bytes_of_json(desc), binary), "not finite"));
}

// Arrays of three positions (36 bytes) with `indices` and the "topology" text `topology` (none: no key).
static std::vector<std::byte> arrays_desc(std::string_view indices, std::string_view topology = {})
{
	std::string text = R"json({"__type":"geometry_desc","__parent":"res://base_geometry.desc","layout":{"position":"vec3"},)json";
	if (!topology.empty()) text += R"json("topology":")json" + std::string(topology) + "\",";
	text += R"json("indices":)json" + std::string(indices);
	text += R"json(,"vertices":[0,0,128,191,0,0,128,191,0,0,0,0,0,0,128,63,0,0,128,191,0,0,0,0,0,0,0,0,0,0,128,63,0,0,0,0]})json";
	return text_bytes(text);
}

BOOST_AUTO_TEST_CASE(TrianglesAsArrays_AndLinesWithABinaryFile_AreRefused)
{
	BOOST_TEST(refused(answer_with_geometry(arrays_desc("[0,1,2]"), std::nullopt), "not lines or points"));
	BOOST_TEST(refused(answer_with_geometry(arrays_desc("[0,1,2]", "mixed"), std::nullopt), "not lines or points"));
	BOOST_TEST(refused(answer_with_geometry(arrays_desc("[0,1,2]", "polygons"), std::nullopt), "not lines or points"));

	const rnd::geometry_files pair = triangle_pair();
	boost::json::object lines = json_of(pair.desc);
	lines[rnd::geometry_topology_key] = "lines";
	BOOST_TEST(refused(answer_with_geometry(bytes_of_json(lines), pair.binary), "lines or points"));
}

BOOST_AUTO_TEST_CASE(LinesAndPoints_InArrays_AreAccepted_WholeLinesOnly_AndIndicesNeedVertices)
{
	std::string problem;
	BOOST_TEST(edt::accept_import_answer(answer_with_geometry(arrays_desc("[0,1,1,2]", "lines"), std::nullopt), request_in(), problem).has_value(), problem);
	BOOST_TEST(edt::accept_import_answer(answer_with_geometry(arrays_desc("[0,1,2]", "points"), std::nullopt), request_in(), problem).has_value(), problem);
	BOOST_TEST(refused(answer_with_geometry(arrays_desc("[0,1,2]", "lines"), std::nullopt), "not whole lines"));

	const std::vector<std::byte> no_vertices = text_bytes(R"json({"__type":"geometry_desc","__parent":"res://base_geometry.desc",)json"
		R"json("layout":{"position":"vec3"},"topology":"points","indices":[0,1,2],"vertices":[]})json");
	BOOST_TEST(refused(answer_with_geometry(no_vertices, std::nullopt), "no vertex to index"));
}

BOOST_AUTO_TEST_SUITE_END()

// The editor's side of a scan through the decoder (#1043), with a fake child: what it hands over, how often, and when it
// stops.
namespace {

struct warn_capture
{
	warn_capture()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) lines.push_back(std::format("{}: {}", category, message));
		});
	}

	std::size_t count(std::string_view part) const
	{
		return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(part) != std::string::npos; }));
	}

	std::vector<std::string> lines;
	engine::log_sink_handle sink;	// last: removed first
};

edt::scan_requests asking(const std::vector<std::string>& side_files)
{
	edt::scan_requests out;
	for (const std::string& name : side_files) out.requested.push_back({ name, edt::model_file_kind::side_file });
	return out;
}

std::shared_ptr<const std::vector<std::byte>> shared_text(std::string_view text)
{
	return std::make_shared<const std::vector<std::byte>>(text_bytes(text));
}

struct broker_env
{
	broker_env()
	{
		dir.write("m/m.obj", "mtllib a.mtl\n");
		dir.write("m/a.mtl", std::string(60, 'a'));
		dir.write("m/b.mtl", std::string(60, 'b'));
		dir.write("m/sub/keep.txt", "x");
		dir.write("outside.mtl", "newmtl outside");
		source.path = dir.path() / "m" / "m.obj";
	}

	test_util::temp_dir dir{ "broker_scan" };
	edt::import_source source;
	int calls = 0;
};

}

BOOST_AUTO_TEST_SUITE(EdtProvidedEntriesTests)

BOOST_AUTO_TEST_CASE(OneFileUnderManyNames_TravelsAsOneBlob)
{
	const auto mtl = shared_text("newmtl x");
	const auto bin = shared_text("buffer");
	const std::vector<edt::provided_file> files = {
		{ "a.mtl", "a.mtl", edt::model_file_kind::side_file, mtl },
		{ "./a.mtl", "a.mtl", edt::model_file_kind::side_file, mtl },
		{ "gone.mtl", "", edt::model_file_kind::side_file, nullptr },
		{ "b.bin", "b.bin", edt::model_file_kind::side_file, bin },
		{ "sub/../a.mtl", "a.mtl", edt::model_file_kind::side_file, mtl } };
	std::vector<edt::bundle_blob> blobs;
	const std::vector<edt::provided_entry> entries = edt::provided_entries(files, 1024, blobs);
	BOOST_REQUIRE_EQUAL(entries.size(), 5u);
	BOOST_REQUIRE_EQUAL(blobs.size(), 2u);
	BOOST_CHECK_EQUAL(blobs[0].tag, edt::provided_blob_tag(0));
	BOOST_CHECK(blobs[0].bytes == *mtl);
	BOOST_CHECK_EQUAL(blobs[1].tag, edt::provided_blob_tag(1));
	BOOST_CHECK(blobs[1].bytes == *bin);
	BOOST_CHECK(entries[0].present && entries[1].present && entries[4].present && entries[3].present);
	BOOST_CHECK_EQUAL(entries[0].blob, 0u);
	BOOST_CHECK_EQUAL(entries[1].blob, 0u);
	BOOST_CHECK_EQUAL(entries[4].blob, 0u);
	BOOST_CHECK_EQUAL(entries[3].blob, 1u);
	BOOST_CHECK(!entries[2].present);
	BOOST_CHECK_EQUAL(entries[2].blob, 0u);
}

BOOST_AUTO_TEST_CASE(TheFilesTogetherOverTheBudget_OrOneFileUnderTooManyNames_AreRefused)
{
	const auto mtl = shared_text("newmtl x");
	std::vector<edt::bundle_blob> blobs;
	// The same bytes under many names count once.
	const std::vector<edt::provided_file> twice = { { "a.mtl", "a.mtl", edt::model_file_kind::side_file, mtl },
		{ "./a.mtl", "a.mtl", edt::model_file_kind::side_file, mtl } };
	BOOST_CHECK_NO_THROW(edt::provided_entries(twice, mtl->size(), blobs));
	const std::vector<edt::provided_file> two_files = { { "a.mtl", "a.mtl", edt::model_file_kind::side_file, mtl },
		{ "b.mtl", "b.mtl", edt::model_file_kind::side_file, shared_text("newmtl y") } };
	blobs.clear();
	BOOST_CHECK_THROW(edt::provided_entries(two_files, mtl->size() + 1, blobs), std::runtime_error);

	std::vector<edt::provided_file> many;
	std::string name = "a.mtl";
	for (std::size_t i = 0; i <= edt::k_max_names_per_file; ++i) {
		many.push_back({ name, "a.mtl", edt::model_file_kind::side_file, mtl });
		name = "./" + name;
	}
	blobs.clear();
	BOOST_CHECK_THROW(edt::provided_entries(many, 1024, blobs), std::runtime_error);
	many.pop_back();
	blobs.clear();
	BOOST_CHECK_NO_THROW(edt::provided_entries(many, 1024, blobs));
	BOOST_CHECK_EQUAL(blobs.size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtBrokerScanTests, broker_env)

BOOST_AUTO_TEST_CASE(AChildThatAsksForANewSideFileEveryRound_FailsAfterThreeRounds)
{
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>&) {
		++calls;
		return asking({ std::format("missing{}.mtl", calls) });
	}, nullptr);
	BOOST_CHECK_EQUAL(calls, 3);
	BOOST_CHECK_MESSAGE(scan.error.find("more than 3 rounds") != std::string::npos, scan.error);
}

BOOST_AUTO_TEST_CASE(ARepeatedRequest_IsAnsweredOnce_AndEndsTheScan)
{
	std::size_t handed = 0;
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>& provided) {
		++calls;
		handed = provided.size();
		return asking({ "a.mtl" });
	}, nullptr);
	BOOST_CHECK_MESSAGE(scan.ok(), scan.error);
	BOOST_CHECK_EQUAL(calls, 2);
	BOOST_CHECK_EQUAL(handed, 1u);
}

BOOST_AUTO_TEST_CASE(TheBudget_CountsOverEveryRound)
{
	source.limits.max_file_bytes = 64;
	source.limits.max_total_bytes = 100;
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>&) {
		++calls;
		return asking({ calls == 1 ? "a.mtl" : "b.mtl" });
	}, nullptr);
	BOOST_CHECK_EQUAL(calls, 2);
	BOOST_CHECK_MESSAGE(scan.error.find("together are bigger than 100 bytes") != std::string::npos, scan.error);
}

BOOST_AUTO_TEST_CASE(ADeadlineThatHasPassed_RunsNoRound)
{
	source.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>&) {
		++calls;
		return asking({});
	}, nullptr);
	BOOST_CHECK_EQUAL(calls, 0);
	BOOST_CHECK_MESSAGE(scan.error.find("took too long") != std::string::npos, scan.error);
}

BOOST_AUTO_TEST_CASE(ADeadlineThatPassesDuringARound_StopsTheScanBeforeTheNext)
{
	source.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>&) {
		++calls;
		source.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
		return asking({ "a.mtl" });
	}, nullptr);
	BOOST_CHECK_EQUAL(calls, 1);
	BOOST_CHECK_MESSAGE(scan.error.find("took too long") != std::string::npos, scan.error);
}

BOOST_AUTO_TEST_CASE(ASideFileTheRuleRefuses_IsMissingToTheChild_WithOneWarning)
{
	const warn_capture warnings;
	std::vector<edt::provided_file> second;
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>& provided) {
		++calls;
		if (calls == 1) return asking({ "../outside.mtl" });
		second = provided;
		return asking({});
	}, nullptr);
	BOOST_CHECK_MESSAGE(scan.ok(), scan.error);
	BOOST_CHECK_EQUAL(calls, 2);
	BOOST_REQUIRE_EQUAL(second.size(), 1u);
	BOOST_CHECK_EQUAL(second[0].name, "../outside.mtl");
	BOOST_CHECK(second[0].bytes == nullptr);
	BOOST_CHECK_EQUAL(warnings.count("side-file rule"), 1u);
	BOOST_CHECK(scan.files.empty());
}

BOOST_AUTO_TEST_CASE(ASideFileOverALimit_FailsTheScan)
{
	const warn_capture warnings;
	source.limits.max_file_bytes = 4;
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>&) {
		++calls;
		return asking({ "a.mtl" });
	}, nullptr);
	BOOST_CHECK_EQUAL(calls, 1);
	BOOST_CHECK_MESSAGE(scan.error.find("too big") != std::string::npos, scan.error);
	BOOST_CHECK_EQUAL(warnings.count("side-file rule"), 0u);
}

BOOST_AUTO_TEST_CASE(OneSideFileUnderManyNames_IsReadOnce_AndSharesOneBlob)
{
	std::vector<edt::provided_file> second;
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>& provided) {
		++calls;
		if (calls == 1) return asking({ "a.mtl", "./a.mtl", "sub/../a.mtl" });
		second = provided;
		return asking({});
	}, nullptr);
	BOOST_REQUIRE_MESSAGE(scan.ok(), scan.error);
	BOOST_REQUIRE_EQUAL(second.size(), 3u);
	BOOST_REQUIRE(second[0].bytes != nullptr);
	BOOST_CHECK(second[1].bytes == second[0].bytes);
	BOOST_CHECK(second[2].bytes == second[0].bytes);
	std::vector<edt::bundle_blob> blobs;
	const std::vector<edt::provided_entry> entries = edt::provided_entries(second, source.limits.max_total_bytes, blobs);
	BOOST_CHECK_EQUAL(entries.size(), 3u);
	BOOST_CHECK_EQUAL(blobs.size(), 1u);
	BOOST_REQUIRE_EQUAL(scan.files.size(), 1u);
	BOOST_CHECK_EQUAL(scan.files[0].names.size(), 3u);
}

BOOST_AUTO_TEST_CASE(OneSideFileUnderMoreThanEightNames_FailsTheScan)
{
	std::vector<std::string> names;
	std::string name = "a.mtl";
	for (std::size_t i = 0; i <= edt::k_max_names_per_file; ++i) {
		names.push_back(name);
		name = "./" + name;
	}
	const edt::model_file_scan scan = edt::broker_scan(source, [&](const std::vector<edt::provided_file>&) {
		++calls;
		return asking(names);
	}, nullptr);
	BOOST_CHECK_MESSAGE(scan.error.find("in more than 8 ways") != std::string::npos, scan.error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtProcessDecoderTests, test_util::scene_env)

// #1077: the decoder has no desc system and stores its prefab unstamped; the editor stamps the prefab blob it accepts with
// its own, and that stamped JSON is what it builds and stores.
BOOST_AUTO_TEST_CASE(AnAcceptedPrefab_IsStampedByTheEditorsDescSystem)
{
	std::string problem;
	const edt::decode_bundle bundle = import_answer();
	const auto as_sent = edt::accept_import_answer(bundle, request_in(), problem);
	BOOST_REQUIRE_MESSAGE(as_sent.has_value(), problem);
	BOOST_TEST(!as_sent->prefab_json.contains("__version"));

	const auto accepted = edt::accept_import_answer(bundle, request_in(), problem, nullptr, descs.get());
	BOOST_REQUIRE_MESSAGE(accepted.has_value(), problem);
	boost::json::object expected = boost::json::parse(R"json({"__type":"prefab_desc","geometry":"memory://m/m.geom.desc"})json").as_object();
	descs->stamp_tree(expected);
	BOOST_TEST(accepted->prefab_json.contains("__version"));
	BOOST_TEST(boost::json::serialize(accepted->prefab_json) == boost::json::serialize(expected));
}

// A prefab blob that is no JSON object is refused when it is to be stamped.
BOOST_AUTO_TEST_CASE(APrefabThatIsNoObject_IsRefused_WhenItIsStamped)
{
	edt::decode_bundle bundle = import_answer();
	bundle.blobs[0].bytes = text_bytes("[1,2]");
	std::string problem;
	BOOST_TEST(!edt::accept_import_answer(bundle, request_in(), problem, nullptr, descs.get()).has_value());
}

BOOST_AUTO_TEST_CASE(AMissingDecoder_IsUnavailable_WithAReasonThatNamesTheFile)
{
	edt::decode_runner_config config;
	config.decoder = fs::temp_directory_path() / "no_such_folder" / "snk_model_decoder";
	const edt::process_decoder decoder(*rs, *descs, config);
	const std::string why = decoder.unavailable_reason();
	BOOST_CHECK_MESSAGE(why.find("snk_model_decoder") != std::string::npos, why);
	BOOST_CHECK_MESSAGE(why.find("not found") != std::string::npos, why);
}

// #1063: each process_decoder asks for the decoder's version; the file is hashed once per process, not per decoder.
BOOST_AUTO_TEST_CASE(TwoDecodersOfOneFile_HashItOnce_AndAChangedFileIsHashedAgain)
{
	test_util::temp_dir dir{ "process_decoder_version" };
	edt::decode_runner_config config;
	config.decoder = dir.write("decoder.bin", "the first build");
	const std::size_t before = edt::file_version_hashes();

	const edt::process_decoder first(*rs, *descs, config);
	const edt::process_decoder second(*rs, *descs, config);
	const std::string version = first.version();
	BOOST_TEST(!version.empty());
	BOOST_TEST(second.version() == version);
	BOOST_TEST(edt::file_version_hashes() == before + 1u);

	// Another build in its place: another size, so another version.
	dir.write("decoder.bin", "the second, longer build");
	const edt::process_decoder third(*rs, *descs, config);
	BOOST_TEST(third.version() != version);
	BOOST_TEST(edt::file_version_hashes() == before + 2u);
}

BOOST_AUTO_TEST_CASE(AnUnreadableDecoder_HasNoVersion)
{
	edt::decode_runner_config config;
	config.decoder = fs::temp_directory_path() / "no_such_folder" / "snk_model_decoder";
	BOOST_TEST(!edt::file_version(config.decoder).has_value());
	BOOST_TEST(edt::process_decoder(*rs, *descs, config).version().empty());
}

#if SNK_HAS_ASSIMP

namespace {

#if !defined(SNK_MODEL_DECODER_PATH) || !defined(SNK_MODEL_DECODER_SELFTEST_PATH)
#error "the process decoder tests need the decoder paths from CMake"
#endif

constexpr const char* k_triangle_obj = "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";

std::vector<std::byte> bytes_of(std::string_view text)
{
	std::vector<std::byte> bytes(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = static_cast<std::byte>(text[i]);
	return bytes;
}

edt::decode_runner_config decoder_config()
{
	edt::decode_runner_config config;
	config.decoder = SNK_MODEL_DECODER_PATH;
	return config;
}

}

BOOST_AUTO_TEST_CASE(AnImportThroughTheProcess_StoresTheSameResourcesAsOneInTheProcess)
{
	test_util::temp_dir dir{ "process_decoder" };
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;
	source.allowed = &nothing;

	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	BOOST_REQUIRE(decoder.unavailable_reason().empty());
	const edt::import_intermediate through_process = decoder.import(source, nullptr);
	BOOST_REQUIRE_MESSAGE(through_process.error.empty(), through_process.error);
	BOOST_REQUIRE(!through_process.created_tags.empty());
	std::vector<std::pair<std::string, std::size_t>> process_blobs;
	for (const res::tag& tag : through_process.created_tags) {
		const auto block = rs->fetch_data(tag);
		BOOST_REQUIRE_MESSAGE(block != nullptr, std::string(tag.view()));
		process_blobs.emplace_back(std::string(tag.view()), block->get().size());
		rs->erase(tag);
	}

	edt::model_importer importer(*descs, *rs);
	const edt::import_intermediate in_process = importer.import_background(source);
	BOOST_REQUIRE_MESSAGE(in_process.error.empty(), in_process.error);
	BOOST_CHECK(in_process.prefab_tag == through_process.prefab_tag);
	BOOST_CHECK(in_process.geom_tag == through_process.geom_tag);
	std::vector<std::pair<std::string, std::size_t>> own_blobs;
	for (const res::tag& tag : in_process.created_tags) {
		const auto block = rs->fetch_data(tag);
		BOOST_REQUIRE(block != nullptr);
		own_blobs.emplace_back(std::string(tag.view()), block->get().size());
	}
	std::sort(process_blobs.begin(), process_blobs.end());
	std::sort(own_blobs.begin(), own_blobs.end());
	BOOST_CHECK(process_blobs == own_blobs);
}

// #1077: the decoder stores the prefab unstamped and the editor stamps it when it accepts the answer: the prefab it stores
// and builds from is byte for byte the one an import in this process stores, versions included.
BOOST_AUTO_TEST_CASE(AnImportThroughTheProcess_StoresTheSamePrefabBytesAsOneInTheProcess)
{
	test_util::temp_dir dir{ "process_decoder" };
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;
	source.allowed = &nothing;

	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	BOOST_REQUIRE(decoder.unavailable_reason().empty());
	const edt::import_intermediate through_process = decoder.import(source, nullptr);
	BOOST_REQUIRE_MESSAGE(through_process.error.empty(), through_process.error);
	BOOST_TEST(through_process.prefab_json.contains("__version"));
	std::vector<std::byte> process_prefab;
	{
		const auto block = rs->fetch_data(through_process.prefab_tag);
		BOOST_REQUIRE(block != nullptr);
		process_prefab = block->get();
	}
	for (const res::tag& tag : through_process.created_tags) rs->erase(tag);

	edt::model_importer importer(*descs, *rs);
	const edt::import_intermediate in_process = importer.import_background(source);
	BOOST_REQUIRE_MESSAGE(in_process.error.empty(), in_process.error);
	BOOST_REQUIRE(in_process.prefab_tag == through_process.prefab_tag);
	const auto block = rs->fetch_data(in_process.prefab_tag);
	BOOST_REQUIRE(block != nullptr);
	BOOST_TEST((block->get() == process_prefab));
	BOOST_TEST(boost::json::serialize(in_process.prefab_json) == boost::json::serialize(through_process.prefab_json));
}

BOOST_AUTO_TEST_CASE(AScanThroughTheProcess_OfASelfContainedModel_ListsNoFiles)
{
	test_util::temp_dir dir{ "process_decoder" };
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;
	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	const edt::model_file_scan scan = decoder.scan(source, nullptr);
	BOOST_CHECK_MESSAGE(scan.ok(), scan.error);
	BOOST_CHECK(scan.files.empty());
}

BOOST_AUTO_TEST_CASE(ASideFileThatChangedAfterItWasListed_FailsTheImportInTheEditor)
{
	test_util::temp_dir dir{ "process_decoder" };
	const std::string obj = "mtllib tri.mtl\nv -1 -1 0\nv 1 -1 0\nv 0 1 0\nusemtl m\nf 1 2 3\n";
	const fs::path model = dir.write("m/tri.obj", obj);
	const fs::path mtl = dir.write("m/tri.mtl", "newmtl m\nKd 1 0 0\n");
	const std::vector<std::byte> bytes = bytes_of(obj);
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;

	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	const edt::model_file_scan listed = decoder.scan(source, nullptr);
	BOOST_REQUIRE_MESSAGE(listed.ok(), listed.error);
	BOOST_REQUIRE_EQUAL(listed.files.size(), 1u);
	BOOST_CHECK(listed.files[0].kind == edt::model_file_kind::side_file);
	BOOST_CHECK(!listed.files[0].names.empty());
	BOOST_CHECK(listed.files[0].hashed);

	// Same size and time, other bytes: only the digest tells.
	const auto written = fs::last_write_time(mtl);
	{
		std::ofstream out(mtl, std::ios::binary | std::ios::trunc);
		out << "newmtl m\nKd 0 1 0\n";
	}
	fs::last_write_time(mtl, written);

	// The editor reads the file again before it hands it over: no decoder runs.
	source.allowed = &listed.files;
	try {
		decoder.import(source, nullptr);
		BOOST_FAIL("a side file that changed was handed to the decoder");
	} catch (const std::runtime_error& e) {
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("changed") != std::string::npos, e.what());
	}
}

BOOST_AUTO_TEST_CASE(ATextureThatAnMtlNamesOutsideTheFolder_FailsTheScanByTheEditorsRule)
{
	test_util::temp_dir dir{ "process_decoder" };
	const std::string obj = "mtllib tri.mtl\nv -1 -1 0\nv 1 -1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl m\nf 1/1 2/2 3/3\n";
	const fs::path model = dir.write("m/tri.obj", obj);
	dir.write("m/tri.mtl", "newmtl m\nKd 1 1 1\nmap_Kd ../secret.png\n");
	dir.write("secret.png", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, 'x'));
	const std::vector<std::byte> bytes = bytes_of(obj);
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;

	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	const edt::model_file_scan scan = decoder.scan(source, nullptr);
	BOOST_CHECK(!scan.ok());
	BOOST_CHECK_MESSAGE(scan.error.find("outside") != std::string::npos, scan.error);
	BOOST_CHECK_MESSAGE(scan.error.find("secret.png") != std::string::npos, scan.error);
	BOOST_CHECK(scan.files.empty());
}

BOOST_AUTO_TEST_CASE(AGltfWithItsBin_IsScannedAndImportedThroughTheEditor)
{
	test_util::temp_dir dir{ "process_decoder" };
	const std::string gltf = R"json({"asset":{"version":"2.0"},"buffers":[{"uri":"tri.bin","byteLength":36}],
		"bufferViews":[{"buffer":0,"byteLength":36}],
		"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]}],
		"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})json";
	const float positions[] = { -1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f };
	const fs::path model = dir.write("m/tri.gltf", gltf);
	dir.write("m/tri.bin", std::string_view(reinterpret_cast<const char*>(positions), sizeof(positions)));
	const std::vector<std::byte> bytes = bytes_of(gltf);
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;

	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	const edt::model_file_scan listed = decoder.scan(source, nullptr);
	BOOST_REQUIRE_MESSAGE(listed.ok(), listed.error);
	BOOST_REQUIRE_EQUAL(listed.files.size(), 1u);
	BOOST_CHECK(listed.files[0].path.filename() == "tri.bin");
	BOOST_CHECK(listed.files[0].kind == edt::model_file_kind::side_file);
	BOOST_CHECK(listed.files[0].hashed);
	BOOST_CHECK_EQUAL(listed.total_bytes, sizeof(positions));

	source.allowed = &listed.files;
	const edt::import_intermediate imported = decoder.import(source, nullptr);
	BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
	BOOST_CHECK(!imported.created_tags.empty());
	for (const res::tag& tag : imported.created_tags) rs->erase(tag);
}

BOOST_AUTO_TEST_CASE(ADecoderThatIsSlow_FailsTheImportWithoutStoringAnything)
{
	const std::vector<std::byte> bytes = bytes_of("x");
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = "x.obj";
	source.preloaded = &bytes;
	source.allowed = &nothing;
	auto config = decoder_config();
	config.decoder = SNK_MODEL_DECODER_SELFTEST_PATH;
	config.timeout = std::chrono::milliseconds(1000);
	config.extra_arguments = { "--self-test-sleep", "60000" };
	const edt::process_decoder decoder(*rs, *descs, config);
	try {
		decoder.import(source, nullptr);
		BOOST_FAIL("a decoder that sleeps was not stopped");
	} catch (const std::runtime_error& e) {
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("too long") != std::string::npos, e.what());
	}
}

// #1043: a decoder that refuses to run without its sandbox (exit code 4) fails the import with its reason.
BOOST_AUTO_TEST_CASE(AnImportThatAsksForTheRequiredSandbox_FailsWithTheReason_WhenTheDecoderCannotHaveIt)
{
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = "x.obj";
	source.preloaded = &bytes;
	source.allowed = &nothing;
	source.sandbox = edt::sandbox_mode::required;
	auto config = decoder_config();
	config.decoder = SNK_MODEL_DECODER_SELFTEST_PATH;
	config.extra_arguments = { "--self-test-pretend-no-landlock" };
	const edt::process_decoder decoder(*rs, *descs, config);
	try {
		decoder.import(source, nullptr);
		BOOST_FAIL("a decoder without its sandbox decoded a model that required it");
	} catch (const std::runtime_error& e) {
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("cannot run in its sandbox") != std::string::npos, e.what());
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("Landlock") != std::string::npos, e.what());
	}
}

// #1043: a sandbox the system has that fails to set up (here: a second thread) fails the import even under `on`, and the
// error says which setting turns the sandbox off.
BOOST_AUTO_TEST_CASE(ASandboxThatFailsToSetUp_FailsTheImport_AndNamesTheSettingThatTurnsItOff)
{
	if (!test_util::k_decoder_has_syscall_filter) {
		BOOST_TEST_MESSAGE("skipped: a sanitizer build does not count its threads");
		return;
	}
#if defined(__linux__)
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = "x.obj";
	source.preloaded = &bytes;
	source.allowed = &nothing;
	auto config = decoder_config();
	config.decoder = SNK_MODEL_DECODER_SELFTEST_PATH;
	config.sandbox = edt::sandbox_mode::best_effort;
	config.extra_arguments = { "--self-test-extra-thread" };
	const edt::process_decoder decoder(*rs, *descs, config);
	try {
		decoder.import(source, nullptr);
		BOOST_FAIL("a decoder whose sandbox failed to set up decoded a model");
	} catch (const std::runtime_error& e) {
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("threads") != std::string::npos, e.what());
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("import/decoder_sandbox") != std::string::npos, e.what());
	}
#endif
}

#if !defined(_WIN32)
// #1076: a child that ran without saying how it is sandboxed counts as having no sandbox, and a run that failed is
// reported all the same (the parse may have run unconfined); one that refused to run parsed nothing and says nothing.
BOOST_AUTO_TEST_CASE(ADecoderThatNeverSaidHowItIsSandboxed_CountsAsUnsandboxed_EvenWhenItFails)
{
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = "x.obj";
	source.preloaded = &bytes;
	source.allowed = &nothing;

	auto silent = decoder_config();
	silent.decoder = "/usr/bin/false";
	std::vector<edt::decoder_sandbox_status> heard;
	const edt::process_decoder failing(*rs, *descs, silent, [&heard](const edt::decoder_sandbox_status& status) { heard.push_back(status); });
	BOOST_CHECK_THROW(failing.import(source, nullptr), std::runtime_error);
	BOOST_REQUIRE_EQUAL(heard.size(), 1u);
	BOOST_CHECK(heard.front().reported);
	BOOST_CHECK(!heard.front().fs);
	BOOST_CHECK_MESSAGE(heard.front().why.find("did not say") != std::string::npos, heard.front().why);

	auto refusing = decoder_config();
	refusing.decoder = SNK_MODEL_DECODER_SELFTEST_PATH;
	refusing.sandbox = edt::sandbox_mode::required;
	refusing.extra_arguments = { "--self-test-pretend-no-landlock" };
	std::vector<edt::decoder_sandbox_status> refused_heard;
	const edt::process_decoder refuser(*rs, *descs, refusing, [&refused_heard](const edt::decoder_sandbox_status& status) {
		refused_heard.push_back(status);
	});
	BOOST_CHECK_THROW(refuser.import(source, nullptr), std::runtime_error);
	BOOST_CHECK(refused_heard.empty());
}
#endif

// #1043: the real decoder decodes real models inside Landlock and the seccomp filter (the CI test of the allowlist).
BOOST_AUTO_TEST_CASE(AGltfWithItsBin_AndAnObjWithItsMtl_DecodeInTheRequiredSandbox)
{
	if (test_util::skipped_without_landlock()) return;
	test_util::temp_dir dir{ "process_decoder_sandbox" };
	const std::string gltf = R"json({"asset":{"version":"2.0"},"buffers":[{"uri":"tri.bin","byteLength":36}],
		"bufferViews":[{"buffer":0,"byteLength":36}],
		"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]}],
		"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})json";
	const float positions[] = { -1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f };
	const std::string obj = "mtllib tri.mtl\nv -1 -1 0\nv 1 -1 0\nv 0 1 0\nusemtl m\nf 1 2 3\n";
	dir.write("g/tri.bin", std::string_view(reinterpret_cast<const char*>(positions), sizeof(positions)));
	dir.write("o/tri.mtl", "newmtl m\nKd 1 0 0\n");
	const std::pair<fs::path, std::string> models[] = { { dir.write("g/tri.gltf", gltf), gltf }, { dir.write("o/tri.obj", obj), obj } };

	const edt::process_decoder decoder(*rs, *descs, decoder_config());
	for (const auto& [model, text] : models) {
		BOOST_TEST_CONTEXT(model.filename().string()) {
			const std::vector<std::byte> bytes = bytes_of(text);
			edt::import_source source;
			source.path = model;
			source.preloaded = &bytes;
			source.sandbox = edt::sandbox_mode::required;
			const edt::model_file_scan listed = decoder.scan(source, nullptr);
			BOOST_REQUIRE_MESSAGE(listed.ok(), listed.error);
			BOOST_CHECK_EQUAL(listed.files.size(), 1u);
			source.allowed = &listed.files;
			const edt::import_intermediate imported = decoder.import(source, nullptr);
			BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
			BOOST_CHECK(!imported.created_tags.empty());
			for (const res::tag& tag : imported.created_tags) rs->erase(tag);
		}
	}
}

BOOST_AUTO_TEST_CASE(ACancelledImport_KillsTheDecoder_AndStoresNothing)
{
	const std::vector<std::byte> bytes = bytes_of("x");
	const std::vector<edt::referenced_file> nothing;
	edt::import_source source;
	source.path = "x.obj";
	source.preloaded = &bytes;
	source.allowed = &nothing;
	auto config = decoder_config();
	config.decoder = SNK_MODEL_DECODER_SELFTEST_PATH;
	config.extra_arguments = { "--self-test-sleep", "60000" };
	const edt::process_decoder decoder(*rs, *descs, config);

	std::stop_source stop;
	ds::task_context ctx;
	ctx.stop = stop.get_token();
	std::jthread stopper([&stop] {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		stop.request_stop();
	});
	BOOST_CHECK_THROW(decoder.import(source, &ctx), ds::task_cancelled);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
