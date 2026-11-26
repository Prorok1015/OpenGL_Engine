#include <boost/test/unit_test.hpp>
#include "edt_decode_codec.h"
#include "edt_model_import_settings.h"
#if SNK_HAS_ASSIMP
#include "edt_decoded_model_adapter.h"
#include "edt_model_import_core.h"
#endif
#include <boost/json.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The settings of a model import (#1195): what the `.resource` of the prefab and the decoder's request hold, read
// strictly; a rewrite keeps what a newer editor of the same version added.

namespace {

namespace json = boost::json;

constexpr std::string_view k_sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

json::value parsed(std::string_view text)
{
	return json::parse(text);
}

bool mentions(const std::string& text, std::string_view part)
{
	return text.find(part) != std::string::npos;
}

}

BOOST_AUTO_TEST_SUITE(EdtModelImportSettings)

BOOST_AUTO_TEST_CASE(NullOrAnEmptyObject_IsTheImportAsBefore)
{
	for (const json::value& value : { json::value{}, parsed("{}"), parsed(R"({"version":1})") }) {
		const edt::model_settings_parse read = edt::parse_model_settings(value);
		BOOST_REQUIRE_MESSAGE(read.ok(), read.error);
		BOOST_CHECK(read.settings == edt::model_import_settings{});
		BOOST_CHECK(read.settings.normals == edt::model_normals::smooth);
		BOOST_CHECK(read.settings.tangents);
		BOOST_CHECK(!read.source.has_value());
	}
}

BOOST_AUTO_TEST_CASE(EveryValue_IsReadBack_AndTheJsonRoundTrips)
{
	const std::string text = R"({"version":1,"source":{"name":"rock.glb","sha256":")" + std::string(k_sha)
		+ R"("},"normals":"flat","tangents":false})";
	const edt::model_settings_parse read = edt::parse_model_settings(parsed(text));
	BOOST_REQUIRE_MESSAGE(read.ok(), read.error);
	BOOST_CHECK(read.settings.normals == edt::model_normals::flat);
	BOOST_CHECK(!read.settings.tangents);
	BOOST_REQUIRE(read.source.has_value());
	BOOST_CHECK_EQUAL(read.source->name, "rock.glb");
	BOOST_CHECK_EQUAL(read.source->sha256, k_sha);

	const json::object written = edt::model_settings_json(read.settings, read.source);
	BOOST_CHECK_EQUAL(written.at("version").as_int64(), edt::k_model_settings_version);
	const edt::model_settings_parse again = edt::parse_model_settings(written);
	BOOST_REQUIRE_MESSAGE(again.ok(), again.error);
	BOOST_CHECK(again.settings == read.settings);
	BOOST_CHECK(again.source == read.source);
	BOOST_CHECK(!edt::model_settings_json(read.settings).contains("source"));
	BOOST_CHECK_EQUAL(edt::model_settings_text(read.settings), "normals flat, tangents off");
	BOOST_CHECK_EQUAL(edt::model_settings_text({}), "normals smooth, tangents on");
}

BOOST_AUTO_TEST_CASE(AnUnknownKey_AWrongTypeOrValue_IsRefusedNamingTheKey)
{
	const auto refusal = [](std::string_view text) {
		const edt::model_settings_parse read = edt::parse_model_settings(parsed(text));
		BOOST_CHECK_MESSAGE(!read.ok(), text);
		return read.error;
	};
	BOOST_CHECK(mentions(refusal(R"({"scale":2})"), "settings.scale"));
	BOOST_CHECK(mentions(refusal(R"({"normals":"round"})"), "settings.normals"));
	BOOST_CHECK(mentions(refusal(R"({"normals":1})"), "settings.normals"));
	BOOST_CHECK(mentions(refusal(R"({"tangents":"yes"})"), "settings.tangents"));
	BOOST_CHECK(mentions(refusal(R"({"version":0})"), "settings.version"));
	BOOST_CHECK(mentions(refusal(R"({"version":"1"})"), "settings.version"));
	BOOST_CHECK(mentions(refusal("[1]"), "object"));
	BOOST_CHECK(mentions(refusal(R"({"source":{"name":"rock.glb"}})"), "settings.source"));
	BOOST_CHECK(mentions(refusal(R"({"source":{"name":"rock.glb","sha256":"ABC"}})"), "settings.source.sha256"));
	const std::string upper(64, 'A');
	BOOST_CHECK(mentions(refusal(R"({"source":{"name":"rock.glb","sha256":")" + upper + R"("}})"), "sha256"));
	const std::string short_hash(63, 'a');
	BOOST_CHECK(mentions(refusal(R"({"source":{"name":"rock.glb","sha256":")" + short_hash + R"("}})"), "sha256"));
	// The source is a file name, never a path of the machine the import ran on.
	for (const std::string_view name : { "/home/me/rock.glb", "C:\\\\models\\\\rock.glb", "C:rock.glb", "..", "" }) {
		const std::string text = std::string(R"({"source":{"name":")") + std::string(name) + R"(","sha256":")"
			+ std::string(k_sha) + R"("}})";
		BOOST_CHECK(mentions(refusal(text), "settings.source.name"));
	}
	BOOST_CHECK(mentions(refusal(R"({"source":{"name":"a","sha256":")" + std::string(k_sha) + R"(","path":"x"}})"),
		"settings.source.path"));
}

BOOST_AUTO_TEST_CASE(AHigherVersion_IsRefusedAsANewerEditors)
{
	const edt::model_settings_parse read = edt::parse_model_settings(parsed(R"({"version":2,"normals":"flat"})"));
	BOOST_REQUIRE(!read.ok());
	BOOST_CHECK_MESSAGE(mentions(read.error, "newer editor"), read.error);
}

BOOST_AUTO_TEST_CASE(ARewrite_KeepsKeysThisEditorDoesNotKnow_AndReplacesItsOwn)
{
	const json::value existing = parsed(R"({"version":1,"normals":"flat","tangents":false,"scale":2.5,)"
		R"("up_axis":{"from":"z"}})");
	const edt::model_source_record source{ "rock.glb", std::string(k_sha) };
	json::object out;
	const std::string why = edt::rewrite_model_settings(existing, {}, source, out);
	BOOST_REQUIRE_MESSAGE(why.empty(), why);
	BOOST_CHECK_EQUAL(out.at("scale").as_double(), 2.5);
	BOOST_CHECK(out.at("up_axis").as_object().at("from").as_string() == "z");
	BOOST_CHECK(out.at("normals").as_string() == "smooth");
	BOOST_CHECK(out.at("tangents").as_bool());
	BOOST_CHECK(out.at("source").as_object().at("name").as_string() == "rock.glb");
	BOOST_CHECK_EQUAL(out.at("version").as_int64(), 1);
	// The rewritten settings read back as a `.resource` is read; an agent's arguments with those keys are refused.
	const edt::model_settings_parse back = edt::parse_model_settings(out, edt::unknown_settings_keys::ignore);
	BOOST_REQUIRE_MESSAGE(back.ok(), back.error);
	BOOST_CHECK(back.settings == edt::model_import_settings{});
	BOOST_REQUIRE(back.source.has_value());
	BOOST_CHECK(*back.source == source);
	BOOST_CHECK(!edt::parse_model_settings(out).ok());
	const edt::model_settings_parse source_key = edt::parse_model_settings(
		parsed(R"({"source":{"name":"a.glb","sha256":")" + std::string(k_sha) + R"(","path":"x"}})"),
		edt::unknown_settings_keys::ignore);
	BOOST_CHECK(!source_key.ok());

	// None there yet: what a new one holds.
	json::object fresh;
	BOOST_REQUIRE(edt::rewrite_model_settings(json::value{}, {}, source, fresh).empty());
	BOOST_CHECK(fresh == edt::model_settings_json({}, source));
}

// A model's file name may hold what a recorded name may not: it is made one, never refused (an export must not fail
// on "scan:v2.glb").
BOOST_AUTO_TEST_CASE(ASourcesFileName_IsMadeAName_TheReaderTakes)
{
	const std::string sha(k_sha);
	const auto reads_back = [&](const edt::model_source_record& source) {
		const edt::model_settings_parse back = edt::parse_model_settings(edt::model_settings_json({}, source));
		BOOST_CHECK_MESSAGE(back.ok(), back.error);
		return back.ok() && back.source && *back.source == source;
	};
	BOOST_CHECK_EQUAL(edt::model_source_of("scan:v2.glb", sha).name, "scan_v2.glb");
	BOOST_CHECK_EQUAL(edt::model_source_of("a\\b/c.glb", sha).name, "a_b_c.glb");
	BOOST_CHECK_EQUAL(edt::model_source_of(std::string("t\x01" "ab\x7f" ".obj"), sha).name, "t_ab_.obj");
	BOOST_CHECK_EQUAL(edt::model_source_of("", sha).name, "model");
	BOOST_CHECK_EQUAL(edt::model_source_of("..", sha).name, "model");
	BOOST_CHECK_EQUAL(edt::model_source_of("rock.glb", sha).name, "rock.glb");
	// Cut on a UTF-8 boundary: a two-byte letter at the limit is not split.
	const std::string longer = std::string(254, 'a') + "\xc3\xa9" + ".glb";
	const edt::model_source_record cut = edt::model_source_of(longer, sha);
	BOOST_CHECK_EQUAL(cut.name.size(), 254u);
	for (const std::string& name : { std::string("scan:v2.glb"), std::string("a\\b/c.glb"), longer,
		std::string("t\x01" "ab.obj"), std::string("."), std::string(300, 'x') }) {
		BOOST_TEST_CONTEXT(name.substr(0, 20))
		{
			BOOST_CHECK(reads_back(edt::model_source_of(name, sha)));
		}
	}
}

BOOST_AUTO_TEST_CASE(ARewriteOfAHigherVersion_IsRefused_AndLeavesTheOutputAlone)
{
	const edt::model_source_record source{ "rock.glb", std::string(k_sha) };
	json::object out{ { "untouched", true } };
	const std::string why = edt::rewrite_model_settings(parsed(R"({"version":3,"lod":4})"), {}, source, out);
	BOOST_CHECK_MESSAGE(mentions(why, "newer editor"), why);
	BOOST_CHECK(out == (json::object{ { "untouched", true } }));
	BOOST_CHECK(!edt::rewrite_model_settings(parsed("[]"), {}, source, out).empty());
}

// `made` (#1194) is the editor's record of what an export wrote: a `.resource` read back takes a valid one, nobody
// gives one.
BOOST_AUTO_TEST_CASE(Made_IsRefusedWhenGiven_AndCheckedWhenReadBack)
{
	const std::string hash(k_sha);
	const json::value good =
		parsed(R"({"made":{"rock.prefab.desc":")" + hash + R"(","textures/a.png":")" + hash + R"("}})");
	const edt::model_settings_parse given = edt::parse_model_settings(good);
	BOOST_CHECK(!given.ok());
	BOOST_CHECK_MESSAGE(mentions(given.error, "recorded by the editor, not given"), given.error);

	const edt::model_settings_parse read = edt::parse_model_settings(good, edt::unknown_settings_keys::ignore);
	BOOST_REQUIRE_MESSAGE(read.ok(), read.error);
	BOOST_REQUIRE(read.made.has_value());
	BOOST_CHECK_EQUAL(read.made->size(), 2u);
	BOOST_CHECK_EQUAL(read.made->at("textures/a.png"), hash);
	BOOST_CHECK(!edt::parse_model_settings(parsed("{}"), edt::unknown_settings_keys::ignore).made.has_value());

	const auto with = [](std::string_view path, std::string_view sha) {
		json::object made;
		made[path] = sha;
		return json::value{ json::object{ { "made", made } } };
	};
	for (const json::value& bad : { parsed(R"({"made":[]})"), with("/abs.png", hash), with("a/../b.png", hash),
		with("..", hash), with("a\\b.png", hash), with("a\nb.png", hash), with(std::string(1025, 'a'), hash),
		with("a.png", "ABC"), with("a.png", std::string(64, 'A')) }) {
		const edt::model_settings_parse refused = edt::parse_model_settings(bad, edt::unknown_settings_keys::ignore);
		BOOST_CHECK_MESSAGE(!refused.ok(), json::serialize(bad));
		BOOST_CHECK(!refused.made.has_value());
	}
	// The rules of check_agent_folder: no drive or protocol, no empty, '.' or '..' segment, no trailing '/', '.' or
	// ' ', no character a file name cannot have, no name Windows reserves for a device.
	for (const char* path : { "c:/a.png", "res://a.png", "a//b.png", "./a.png", "a/./b.png", "a/", "a/b.", "a/b ",
		"a<b.png", "a|b.png", "a?b.png", "a*b.png", "a\"b.png", "CON.png", "nul/x.png", "textures/COM1", "LPT9.txt" }) {
		const edt::model_settings_parse refused = edt::parse_model_settings(with(path, hash),
			edt::unknown_settings_keys::ignore);
		BOOST_CHECK_MESSAGE(!refused.ok(), path);
	}
	BOOST_CHECK(edt::parse_model_settings(with("textures/con_tex.png", hash), edt::unknown_settings_keys::ignore).ok());
	json::object many;
	for (std::size_t i = 0; i <= edt::k_max_made_entries; ++i) many[std::to_string(i)] = hash;
	BOOST_CHECK(!edt::parse_model_settings(json::object{ { "made", many } }, edt::unknown_settings_keys::ignore).ok());
}

BOOST_AUTO_TEST_CASE(ARewrite_DropsTheMadeOfTheEarlierExport)
{
	const std::string hash(k_sha);
	const json::value existing = parsed(R"({"version":1,"lod":4,"made":{"a.png":")" + hash + R"("}})");
	json::object out;
	BOOST_REQUIRE(edt::rewrite_model_settings(existing, {}, { "rock.glb", hash }, out).empty());
	BOOST_CHECK(!out.contains("made"));
	BOOST_CHECK(out.contains("lod"));
}

BOOST_AUTO_TEST_CASE(TheDecodersRequest_CarriesTheSettings_AndRefusesBadOnes)
{
	edt::decode_request request;
	request.mode = edt::decode_request::mode_kind::import;
	request.source_path = "rock.glb";
	request.settings = { edt::model_normals::flat, false };
	const json::object written = edt::to_json(request);
	const std::optional<edt::decode_request> read = edt::decode_request_from_json(written);
	BOOST_REQUIRE(read.has_value());
	BOOST_CHECK(read->settings == request.settings);

	// A request of an editor without them is read as the defaults.
	json::object without = written;
	without.erase("settings");
	const std::optional<edt::decode_request> defaults = edt::decode_request_from_json(without);
	BOOST_REQUIRE(defaults.has_value());
	BOOST_CHECK(defaults->settings == edt::model_import_settings{});

	const std::string with_source = R"({"source":{"name":"rock.glb","sha256":")" + std::string(k_sha) + R"("}})";
	for (const std::string_view bad : { std::string_view(R"({"normals":"round"})"), std::string_view(R"({"lod":1})"),
			std::string_view(R"({"version":2})"), std::string_view("null"), std::string_view("[]"),
			std::string_view(with_source) }) {
		json::object refused = written;
		refused["settings"] = parsed(bad);
		BOOST_CHECK_MESSAGE(!edt::decode_request_from_json(refused).has_value(), bad);
	}
}

#if SNK_HAS_ASSIMP
BOOST_AUTO_TEST_CASE(TheSettings_AreWhatTheImportIsTold)
{
	const mdl::mesh_processing defaults = edt::mesh_processing_of({});
	BOOST_CHECK(defaults.normals == mdl::normals_mode::smooth);
	BOOST_CHECK(defaults.tangents);
	const mdl::mesh_processing flat = edt::mesh_processing_of({ edt::model_normals::flat, false });
	BOOST_CHECK(flat.normals == mdl::normals_mode::flat);
	BOOST_CHECK(!flat.tangents);
}

BOOST_AUTO_TEST_CASE(TheDecodeCacheKey_DiffersByTheSettings)
{
	const std::vector<std::byte> model(16, std::byte{ 7 });
	edt::import_source source;
	source.path = "models/rock.glb";
	const std::string defaults = edt::decoded_model_adapter::cache_key("v1", model, source);
	BOOST_CHECK_EQUAL(defaults, edt::decoded_model_adapter::cache_key("v1", model, source));

	source.settings.normals = edt::model_normals::flat;
	const std::string flat = edt::decoded_model_adapter::cache_key("v1", model, source);
	source.settings = { edt::model_normals::smooth, false };
	const std::string no_tangents = edt::decoded_model_adapter::cache_key("v1", model, source);
	BOOST_CHECK_NE(defaults, flat);
	BOOST_CHECK_NE(defaults, no_tangents);
	BOOST_CHECK_NE(flat, no_tangents);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
