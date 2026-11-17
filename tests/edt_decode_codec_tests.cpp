#include <boost/test/unit_test.hpp>
#include "edt_decode_codec.h"
#include <boost/json/serialize.hpp>
#include <string>
#include <vector>

// What the decoder process and the editor exchange besides the resources (#647): the files the editor hands over (#1043),
// what a scan asks for and an import's fields. The readers refuse a form another process could write wrongly or on purpose.

namespace {

edt::referenced_file sample_file()
{
	edt::referenced_file file;
	file.path = std::filesystem::path("some") / "dir" / "skin.png";
	file.size = 18446744073709551000ull;
	file.modified = std::filesystem::file_time_type(std::filesystem::file_time_type::duration(-123456789));
	file.id.volume = 7;
	file.id.index = 18446744073709551615ull;
	file.id.index_high = 3;
	file.id.valid = true;
	file.hashed = true;
	for (std::size_t i = 0; i < file.digest.size(); ++i) file.digest[i] = static_cast<std::uint8_t>(i * 8 + 1);
	return file;
}

}

BOOST_AUTO_TEST_SUITE(EdtDecodeCodecTests)

BOOST_AUTO_TEST_CASE(AReferencedFile_RoundTrips_WithItsIdentityAndDigest)
{
	const edt::referenced_file file = sample_file();
	const auto back = edt::referenced_file_from_json(edt::to_json(file));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back->path == file.path);
	BOOST_CHECK_EQUAL(back->size, file.size);
	BOOST_CHECK(back->modified == file.modified);
	BOOST_CHECK(back->id == file.id);
	BOOST_CHECK(back->digest == file.digest);
	BOOST_CHECK(back->hashed);
}

BOOST_AUTO_TEST_CASE(AReferencedFile_WithAWrongField_IsRefused)
{
	for (const char* field : { "path", "size", "modified", "volume", "index", "index_high", "id_valid", "hashed", "digest" }) {
		BOOST_TEST_CONTEXT(field) {
			boost::json::object broken = edt::to_json(sample_file());
			broken.erase(field);
			BOOST_CHECK(!edt::referenced_file_from_json(broken).has_value());
		}
	}
	boost::json::object bad_number = edt::to_json(sample_file());
	bad_number["size"] = "12x";
	BOOST_CHECK(!edt::referenced_file_from_json(bad_number).has_value());
	boost::json::object bad_digest = edt::to_json(sample_file());
	bad_digest["digest"] = "abcd";
	BOOST_CHECK(!edt::referenced_file_from_json(bad_digest).has_value());
	BOOST_CHECK(!edt::referenced_file_from_json(boost::json::value(5)).has_value());
}

BOOST_AUTO_TEST_CASE(AScan_RoundTrips)
{
	edt::model_file_scan scan;
	scan.files.push_back(sample_file());
	scan.total_bytes = 99;
	scan.error = "";
	const auto back = edt::model_file_scan_from_json(edt::to_json(scan));
	BOOST_REQUIRE(back.has_value());
	BOOST_REQUIRE_EQUAL(back->files.size(), 1u);
	BOOST_CHECK_EQUAL(back->total_bytes, 99u);
	BOOST_CHECK(back->error.empty());

	scan.error = "the model cannot be read: nope";
	BOOST_CHECK_EQUAL(edt::model_file_scan_from_json(edt::to_json(scan))->error, scan.error);
}

BOOST_AUTO_TEST_CASE(AnImport_RoundTrips_AndTakesNoTagButMemory)
{
	edt::import_intermediate imported;
	imported.geom_tag = res::tag("memory://robot/robot.geom.desc");
	imported.prefab_tag = res::tag("memory://robot/robot.prefab.desc");
	imported.all_tags = { imported.geom_tag, imported.prefab_tag, res::tag("memory://robot/tex/albedo.png") };
	imported.created_tags = { imported.geom_tag, imported.prefab_tag };
	imported.source_path = std::filesystem::path("some") / "robot.glb";
	imported.prefab_json["name"] = "robot";
	imported.skin_weights = { { 1, 2, 4000000000u }, {} };
	imported.missing_textures = { "lost.png" };

	const auto back = edt::import_intermediate_from_json(edt::to_json(imported));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK_EQUAL(back->geom_tag.string(), imported.geom_tag.string());
	BOOST_CHECK_EQUAL(back->created_tags.size(), 2u);
	BOOST_CHECK_EQUAL(back->all_tags.size(), 3u);
	BOOST_CHECK(back->source_path == imported.source_path);
	BOOST_CHECK_EQUAL(boost::json::serialize(back->prefab_json), boost::json::serialize(imported.prefab_json));
	BOOST_REQUIRE_EQUAL(back->skin_weights.size(), 2u);
	BOOST_CHECK_EQUAL(back->skin_weights[0][2], 4000000000u);
	BOOST_CHECK((back->missing_textures == std::vector<std::string>{ "lost.png" }));

	for (const char* bad : { "res://robot.prefab.desc", "memory://../x", "user://settings" }) {
		BOOST_TEST_CONTEXT(bad) {
			boost::json::object object = edt::to_json(imported);
			object["created_tags"] = boost::json::array{ bad };
			BOOST_CHECK(!edt::import_intermediate_from_json(object).has_value());
		}
	}
	boost::json::object weights = edt::to_json(imported);
	weights["skin_weights"] = boost::json::array{ boost::json::array{ -1 } };
	BOOST_CHECK(!edt::import_intermediate_from_json(weights).has_value());
	weights["skin_weights"] = boost::json::array{ boost::json::array{ 5000000000ull } };
	BOOST_CHECK(!edt::import_intermediate_from_json(weights).has_value());
}

// #1108: the importer lists at most 256 missing textures of at most 1024 bytes each and counts the rest; a longer list is
// not the importer's answer.
BOOST_AUTO_TEST_CASE(MissingTextures_RoundTripWithTheirCount_AndAListOverTheLimitIsRefused)
{
	edt::import_intermediate imported;
	imported.geom_tag = res::tag("memory://m/m.geom.desc");
	imported.prefab_tag = res::tag("memory://m/m.prefab.desc");
	imported.missing_textures = std::vector<std::string>(256, "lost.png");
	imported.missing_textures_not_listed = 7;
	const auto back = edt::import_intermediate_from_json(edt::to_json(imported));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK_EQUAL(back->missing_textures.size(), 256u);
	BOOST_CHECK_EQUAL(back->missing_textures_not_listed, 7u);

	boost::json::object too_many = edt::to_json(imported);
	too_many["missing_textures"].as_array().emplace_back("one.png");
	BOOST_CHECK(!edt::import_intermediate_from_json(too_many).has_value());

	boost::json::object too_long = edt::to_json(imported);
	too_long["missing_textures"] = boost::json::array{ std::string(1025, 'x') };
	BOOST_CHECK(!edt::import_intermediate_from_json(too_long).has_value());

	boost::json::object empty_name = edt::to_json(imported);
	empty_name["missing_textures"] = boost::json::array{ "" };
	BOOST_CHECK(!edt::import_intermediate_from_json(empty_name).has_value());

	boost::json::object negative = edt::to_json(imported);
	negative["missing_textures_not_listed"] = -1;
	BOOST_CHECK(!edt::import_intermediate_from_json(negative).has_value());
}

BOOST_AUTO_TEST_CASE(ADigest_RoundTripsThroughHex_AndRefusesBadText)
{
	const edt::referenced_file file = sample_file();
	const std::string hex = edt::digest_to_hex(file.digest);
	BOOST_CHECK_EQUAL(hex.size(), 64u);
	BOOST_CHECK(edt::digest_from_hex(hex).value() == file.digest);
	BOOST_CHECK(!edt::digest_from_hex(hex.substr(1)).has_value());
	BOOST_CHECK(!edt::digest_from_hex(std::string(64, 'g')).has_value());
}

BOOST_AUTO_TEST_CASE(ARequest_RoundTrips_AndRefusesAWrongForm)
{
	edt::decode_request request;
	request.mode = edt::decode_request::mode_kind::import;
	request.source_path = std::filesystem::path("some") / "model.obj";
	request.provided.push_back({ "../tex/skin.png", "tex/skin.png", edt::model_file_kind::texture, true, 3 });
	request.provided.push_back({ "/abs/models/missing.bin", "", edt::model_file_kind::side_file, false, 0 });
	request.provided.push_back({ "./../tex/skin.png", "tex/skin.png", edt::model_file_kind::texture, true, 3 });
	request.limits.max_files = 7;
	request.limits.max_file_bytes = 18446744073709551000ull;
	request.deadline_ms = 90000;
	request.memory_limit_bytes = 2147483648ull;
	request.boundary = std::filesystem::path("some") / "content";
	request.memory_folder = "__models/res/some/model/";
	request.on_disk = false;

	const auto back = edt::decode_request_from_json(edt::to_json(request));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back->mode == edt::decode_request::mode_kind::import);
	BOOST_CHECK(back->source_path == request.source_path);
	BOOST_REQUIRE_EQUAL(back->provided.size(), 3u);
	BOOST_CHECK_EQUAL(back->provided[0].blob, 3u);
	BOOST_CHECK_EQUAL(back->provided[2].blob, 3u);
	BOOST_CHECK_EQUAL(back->provided[1].blob, 0u);
	BOOST_CHECK_EQUAL(back->provided[0].name, "../tex/skin.png");
	BOOST_CHECK_EQUAL(back->provided[0].relative, "tex/skin.png");
	BOOST_CHECK(back->provided[0].kind == edt::model_file_kind::texture);
	BOOST_CHECK(back->provided[0].present);
	BOOST_CHECK(back->provided[1].kind == edt::model_file_kind::side_file);
	BOOST_CHECK(!back->provided[1].present);
	BOOST_CHECK(back->provided[1].relative.empty());
	BOOST_CHECK_EQUAL(back->limits.max_files, 7u);
	BOOST_CHECK_EQUAL(back->limits.max_file_bytes, request.limits.max_file_bytes);
	BOOST_CHECK_EQUAL(back->deadline_ms, 90000u);
	BOOST_CHECK_EQUAL(back->memory_limit_bytes, request.memory_limit_bytes);
	BOOST_CHECK(back->boundary == request.boundary);
	BOOST_CHECK_EQUAL(back->memory_folder, request.memory_folder);
	BOOST_CHECK(!back->on_disk);

	boost::json::object bad_mode = edt::to_json(request);
	bad_mode["mode"] = "run";
	BOOST_CHECK(!edt::decode_request_from_json(bad_mode).has_value());
	boost::json::object no_source = edt::to_json(request);
	no_source["source_path"] = "";
	BOOST_CHECK(!edt::decode_request_from_json(no_source).has_value());
	boost::json::object number_limit = edt::to_json(request);
	number_limit["deadline_ms"] = 5;
	BOOST_CHECK(!edt::decode_request_from_json(number_limit).has_value());
	boost::json::object no_folder = edt::to_json(request);
	no_folder.erase("memory_folder");
	BOOST_CHECK(!edt::decode_request_from_json(no_folder).has_value());
	boost::json::object no_boundary = edt::to_json(request);
	no_boundary.erase("boundary");
	BOOST_CHECK(!edt::decode_request_from_json(no_boundary).has_value());
	boost::json::object text_on_disk = edt::to_json(request);
	text_on_disk["on_disk"] = "false";
	BOOST_CHECK(!edt::decode_request_from_json(text_on_disk).has_value());
	BOOST_CHECK(!edt::decode_request_from_json(boost::json::value(3)).has_value());
	boost::json::object no_provided = edt::to_json(request);
	no_provided.erase("provided");
	BOOST_CHECK(!edt::decode_request_from_json(no_provided).has_value());
}

BOOST_AUTO_TEST_CASE(ARequest_WithAProvidedFileOfABadForm_IsRefused)
{
	edt::decode_request request;
	request.source_path = "model.obj";
	request.provided.push_back({ "model.mtl", "model.mtl", edt::model_file_kind::side_file, true });
	BOOST_REQUIRE(edt::decode_request_from_json(edt::to_json(request)).has_value());

	const auto with_entry = [&](const char* key, const auto& value) {
		boost::json::object object = edt::to_json(request);
		object["provided"].as_array()[0].as_object()[key] = value;
		return edt::decode_request_from_json(object);
	};
	BOOST_CHECK(!with_entry("name", std::string(1025, 'a')).has_value());
	BOOST_CHECK(with_entry("name", std::string(1024, 'a')).has_value());
	BOOST_CHECK(!with_entry("name", "").has_value());
	BOOST_CHECK(!with_entry("name", std::string("a\0b", 3)).has_value());
	BOOST_CHECK(!with_entry("relative", std::string(1025, 'a')).has_value());
	BOOST_CHECK(!with_entry("relative", "").has_value());
	BOOST_CHECK(!with_entry("kind", "model").has_value());
	BOOST_CHECK(!with_entry("kind", 1).has_value());
	BOOST_CHECK(!with_entry("present", "true").has_value());
	BOOST_CHECK(!with_entry("blob", 1).has_value());
	BOOST_CHECK(!with_entry("blob", "-1").has_value());
	BOOST_CHECK(!with_entry("blob", "4294967296").has_value());
	BOOST_CHECK(with_entry("blob", "4294967295").has_value());
	{
		boost::json::object object = edt::to_json(request);
		object["provided"].as_array()[0].as_object().erase("blob");
		BOOST_CHECK(!edt::decode_request_from_json(object).has_value());
	}

	// A file that is not on the disk has no name below the boundary.
	request.provided[0].present = false;
	BOOST_CHECK(!edt::decode_request_from_json(edt::to_json(request)).has_value());
	request.provided[0].relative.clear();
	BOOST_CHECK(edt::decode_request_from_json(edt::to_json(request)).has_value());
	// Nor bytes of its own.
	request.provided[0].blob = 1;
	BOOST_CHECK(!edt::decode_request_from_json(edt::to_json(request)).has_value());
}

BOOST_AUTO_TEST_CASE(TheRequestsOfAScan_RoundTrip_AndAnErrorStandsAlone)
{
	edt::scan_requests requests;
	requests.requested.push_back({ "m.mtl", edt::model_file_kind::side_file });
	requests.requested.push_back({ "skin.png", edt::model_file_kind::texture });
	const auto back = edt::scan_requests_from_json(edt::to_json(requests));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back->error.empty());
	BOOST_REQUIRE_EQUAL(back->requested.size(), 2u);
	BOOST_CHECK_EQUAL(back->requested[0].name, "m.mtl");
	BOOST_CHECK(back->requested[0].kind == edt::model_file_kind::side_file);
	BOOST_CHECK(back->requested[1].kind == edt::model_file_kind::texture);

	edt::scan_requests failed;
	failed.error = "the model cannot be read: nope";
	BOOST_CHECK_EQUAL(edt::scan_requests_from_json(edt::to_json(failed))->error, failed.error);

	boost::json::object bad_kind = edt::to_json(requests);
	bad_kind["requests"].as_array()[0].as_object()["kind"] = "anything";
	BOOST_CHECK(!edt::scan_requests_from_json(bad_kind).has_value());
	BOOST_CHECK(!edt::scan_requests_from_json(boost::json::object{}).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
