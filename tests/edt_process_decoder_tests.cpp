#include <boost/test/unit_test.hpp>
#include "edt_process_decoder.h"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

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

edt::referenced_file file_at(const fs::path& path, std::uintmax_t size = 10)
{
	edt::referenced_file file;
	file.path = path;
	file.size = size;
	return file;
}

edt::decode_bundle scan_answer(const std::vector<edt::referenced_file>& files)
{
	edt::model_file_scan scan;
	scan.files = files;
	edt::decode_bundle bundle;
	bundle.manifest["kind"] = "scan";
	bundle.manifest["scan"] = edt::to_json(scan);
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

BOOST_AUTO_TEST_CASE(AScanAnswer_WithFilesInTheFolderOfTheModel_IsAccepted_ElseRefused)
{
	const auto request = request_in();
	std::string problem;
	const auto good = edt::accept_scan_answer(scan_answer({ file_at(k_models / "a.mtl"), file_at(k_models / "tex" / "b.png", 20) }), request, problem);
	BOOST_REQUIRE_MESSAGE(good.has_value(), problem);
	BOOST_CHECK_EQUAL(good->files.size(), 2u);
	BOOST_CHECK_EQUAL(good->total_bytes, 30u);

	BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ file_at(k_models.parent_path() / "other" / "a.mtl") }), request, problem).has_value());
	BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ file_at(k_models / ".." / "a.mtl") }), request, problem).has_value());
	BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ file_at(k_models / "a.mtl"), file_at(k_models / "a.mtl") }), request, problem).has_value());
}

BOOST_AUTO_TEST_CASE(AScanAnswer_OverTheLimitsOfTheRequest_OrWithResources_IsRefused)
{
	auto request = request_in();
	request.limits.max_files = 1;
	std::string problem;
	BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ file_at(k_models / "a"), file_at(k_models / "b") }), request, problem).has_value());

	request = request_in();
	request.limits.max_file_bytes = 5;
	BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ file_at(k_models / "a", 6) }), request, problem).has_value());

	request = request_in();
	request.limits.max_total_bytes = 15;
	BOOST_CHECK(!edt::accept_scan_answer(scan_answer({ file_at(k_models / "a", 10), file_at(k_models / "b", 10) }), request, problem).has_value());

	auto with_blob = scan_answer({});
	with_blob.blobs.push_back({ "memory://m/x.desc", { std::byte{ 1 } } });
	BOOST_CHECK(!edt::accept_scan_answer(with_blob, request_in(), problem).has_value());
}

BOOST_AUTO_TEST_CASE(AScanThatTheChildReportedAsFailed_ComesBackWithItsError)
{
	edt::decode_bundle failed;
	failed.manifest["kind"] = "scan";
	failed.manifest["error"] = "std::bad_alloc";
	std::string problem;
	const auto scan = edt::accept_scan_answer(failed, request_in(), problem);
	BOOST_REQUIRE(scan.has_value());
	BOOST_CHECK(!scan->ok());
	BOOST_CHECK_EQUAL(scan->error, "the decoder ran out of memory");
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

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtProcessDecoderTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(AMissingDecoder_IsUnavailable_WithAReasonThatNamesTheFile)
{
	edt::decode_runner_config config;
	config.decoder = fs::temp_directory_path() / "no_such_folder" / "snk_model_decoder";
	const edt::process_decoder decoder(*rs, config);
	const std::string why = decoder.unavailable_reason();
	BOOST_CHECK_MESSAGE(why.find("snk_model_decoder") != std::string::npos, why);
	BOOST_CHECK_MESSAGE(why.find("not found") != std::string::npos, why);
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

	const edt::process_decoder decoder(*rs, decoder_config());
	BOOST_REQUIRE(decoder.unavailable_reason().empty());
	const edt::import_intermediate through_process = decoder.import(source, nullptr);
	BOOST_REQUIRE_MESSAGE(through_process.error.empty(), through_process.error);
	BOOST_REQUIRE(!through_process.created_tags.empty());
	std::vector<std::pair<std::string, std::size_t>> process_blobs;
	for (const res::tag& tag : through_process.created_tags) {
		const auto block = rs->fetch_data(tag);
		BOOST_REQUIRE_MESSAGE(block.has_value(), std::string(tag.view()));
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
		BOOST_REQUIRE(block.has_value());
		own_blobs.emplace_back(std::string(tag.view()), block->get().size());
	}
	std::sort(process_blobs.begin(), process_blobs.end());
	std::sort(own_blobs.begin(), own_blobs.end());
	BOOST_CHECK(process_blobs == own_blobs);
}

BOOST_AUTO_TEST_CASE(AScanThroughTheProcess_OfASelfContainedModel_ListsNoFiles)
{
	test_util::temp_dir dir{ "process_decoder" };
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const std::vector<std::byte> bytes = bytes_of(k_triangle_obj);
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;
	const edt::process_decoder decoder(*rs, decoder_config());
	const edt::model_file_scan scan = decoder.scan(source, nullptr);
	BOOST_CHECK_MESSAGE(scan.ok(), scan.error);
	BOOST_CHECK(scan.files.empty());
}

BOOST_AUTO_TEST_CASE(ASideFileThatChangedAfterItWasListed_FailsTheImportInTheChild)
{
	test_util::temp_dir dir{ "process_decoder" };
	const std::string obj = "mtllib tri.mtl\nv -1 -1 0\nv 1 -1 0\nv 0 1 0\nusemtl m\nf 1 2 3\n";
	const fs::path model = dir.write("m/tri.obj", obj);
	const fs::path mtl = dir.write("m/tri.mtl", "newmtl m\nKd 1 0 0\n");
	const std::vector<std::byte> bytes = bytes_of(obj);
	edt::import_source source;
	source.path = model;
	source.preloaded = &bytes;

	const edt::process_decoder decoder(*rs, decoder_config());
	const edt::model_file_scan listed = decoder.scan(source, nullptr);
	BOOST_REQUIRE_MESSAGE(listed.ok(), listed.error);
	BOOST_REQUIRE_EQUAL(listed.files.size(), 1u);

	// Same size and time, other bytes: only the digest tells.
	const auto written = fs::last_write_time(mtl);
	{
		std::ofstream out(mtl, std::ios::binary | std::ios::trunc);
		out << "newmtl m\nKd 0 1 0\n";
	}
	fs::last_write_time(mtl, written);

	source.allowed = &listed.files;
	const edt::import_intermediate imported = decoder.import(source, nullptr);
	BOOST_CHECK(!imported.error.empty());
	BOOST_CHECK_MESSAGE(imported.error.find("changed") != std::string::npos, imported.error);
	BOOST_CHECK(imported.created_tags.empty());
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
	const edt::process_decoder decoder(*rs, config);
	try {
		decoder.import(source, nullptr);
		BOOST_FAIL("a decoder that sleeps was not stopped");
	} catch (const std::runtime_error& e) {
		BOOST_CHECK_MESSAGE(std::string(e.what()).find("too long") != std::string::npos, e.what());
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
	const edt::process_decoder decoder(*rs, config);

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
