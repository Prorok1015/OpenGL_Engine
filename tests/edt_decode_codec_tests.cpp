#include <boost/test/unit_test.hpp>
#include "edt_decode_codec.h"
#include <boost/json/serialize.hpp>

// What the decoder process and the editor exchange besides the resources (#647): the files a model may read, a scan and
// an import's fields. The readers refuse a form another process could write wrongly or on purpose.

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

	const auto back = edt::import_intermediate_from_json(edt::to_json(imported));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK_EQUAL(back->geom_tag.string(), imported.geom_tag.string());
	BOOST_CHECK_EQUAL(back->created_tags.size(), 2u);
	BOOST_CHECK_EQUAL(back->all_tags.size(), 3u);
	BOOST_CHECK(back->source_path == imported.source_path);
	BOOST_CHECK_EQUAL(boost::json::serialize(back->prefab_json), boost::json::serialize(imported.prefab_json));
	BOOST_REQUIRE_EQUAL(back->skin_weights.size(), 2u);
	BOOST_CHECK_EQUAL(back->skin_weights[0][2], 4000000000u);

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
	request.allowed.push_back(sample_file());
	request.limits.max_files = 7;
	request.limits.max_file_bytes = 18446744073709551000ull;
	request.deadline_ms = 90000;
	request.memory_limit_bytes = 2147483648ull;

	const auto back = edt::decode_request_from_json(edt::to_json(request));
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back->mode == edt::decode_request::mode_kind::import);
	BOOST_CHECK(back->source_path == request.source_path);
	BOOST_REQUIRE_EQUAL(back->allowed.size(), 1u);
	BOOST_CHECK(back->allowed[0].digest == request.allowed[0].digest);
	BOOST_CHECK_EQUAL(back->limits.max_files, 7u);
	BOOST_CHECK_EQUAL(back->limits.max_file_bytes, request.limits.max_file_bytes);
	BOOST_CHECK_EQUAL(back->deadline_ms, 90000u);
	BOOST_CHECK_EQUAL(back->memory_limit_bytes, request.memory_limit_bytes);

	boost::json::object bad_mode = edt::to_json(request);
	bad_mode["mode"] = "run";
	BOOST_CHECK(!edt::decode_request_from_json(bad_mode).has_value());
	boost::json::object no_source = edt::to_json(request);
	no_source["source_path"] = "";
	BOOST_CHECK(!edt::decode_request_from_json(no_source).has_value());
	boost::json::object number_limit = edt::to_json(request);
	number_limit["deadline_ms"] = 5;
	BOOST_CHECK(!edt::decode_request_from_json(number_limit).has_value());
	BOOST_CHECK(!edt::decode_request_from_json(boost::json::value(3)).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
