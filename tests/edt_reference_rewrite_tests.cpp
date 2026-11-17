#include <boost/test/unit_test.hpp>
#include "edt_reference_rewrite.h"
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Rewriting the references to a file by JSON pointer (#775): only a file that writes back unchanged is edited.
namespace {

namespace json = boost::json;

std::string canonical(std::string_view text)
{
	const std::vector<std::byte> bytes = res::pretty_json_bytes(json::parse(text));
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const char* const k_document = R"({
	"__type": "level_desc",
	"sky": "res://sky.desc",
	"mesh": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://old_sky.desc" },
	"by_path": { "path": "res://sky.desc" },
	"guid_only": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f" },
	"list": [ "res://sky.desc", "res://other.desc" ]
})";

const std::vector<std::string> k_pointers{ "/sky", "/mesh", "/by_path", "/guid_only", "/list/0" };

}

BOOST_AUTO_TEST_SUITE(EdtReferenceRewriteTests)

BOOST_AUTO_TEST_CASE(AStringAndAnObject_GetTheNewPath_TheGuidIsKept)
{
	const std::string text = canonical(k_document);
	const edt::rewrite_outcome outcome = edt::rewrite_references(text, k_pointers, "res://sky.desc", "res://env/clouds.desc", false);
	BOOST_REQUIRE_MESSAGE((outcome.status == edt::rewrite_status::rewritten), outcome.reason);
	BOOST_TEST(outcome.pointers == (std::vector<std::string>{ "/sky", "/mesh", "/by_path", "/list/0" }),
		"a {guid} without a path holds nothing to change");

	const json::value after = json::parse(outcome.text);
	BOOST_TEST(std::string(after.at("sky").as_string().c_str()) == "res://env/clouds.desc");
	BOOST_TEST(std::string(after.at("mesh").at("path").as_string().c_str()) == "res://env/clouds.desc");
	BOOST_TEST(std::string(after.at("mesh").at("guid").as_string().c_str()) == "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	BOOST_TEST(std::string(after.at("by_path").at("path").as_string().c_str()) == "res://env/clouds.desc");
	BOOST_TEST(std::string(after.at("list").as_array().at(1).as_string().c_str()) == "res://other.desc", "what was not asked for stays");
	BOOST_TEST(outcome.text == canonical(outcome.text), "the result is in the project's layout too");
}

BOOST_AUTO_TEST_CASE(AFileThatDoesNotWriteBackUnchanged_IsNotTouched)
{
	const std::string compact = R"({"__type":"level_desc","sky":"res://sky.desc"})";
	const edt::rewrite_outcome outcome = edt::rewrite_references(compact, std::vector<std::string>{ "/sky" }, "res://sky.desc", "res://b.desc", false);
	BOOST_TEST((outcome.status == edt::rewrite_status::not_canonical));
	BOOST_TEST(outcome.text.empty());
	BOOST_TEST(outcome.pointers.empty());
}

BOOST_AUTO_TEST_CASE(NamesAreComparedInTheCaseTheDatabaseUses)
{
	const std::string text = canonical(R"({ "sky": "res://Sky.desc" })");
	const std::vector<std::string> pointers{ "/sky" };
	BOOST_TEST((edt::rewrite_references(text, pointers, "res://sky.desc", "res://b.desc", true).status == edt::rewrite_status::rewritten));
	BOOST_TEST((edt::rewrite_references(text, pointers, "res://sky.desc", "res://b.desc", false).status == edt::rewrite_status::not_a_reference));
}

BOOST_AUTO_TEST_CASE(APointerThatLeadsNowhereOrToAnythingElse_FailsTheWholeFile)
{
	const std::string text = canonical(R"({ "sky": "res://sky.desc", "count": 3, "other": "res://other.desc" })");
	BOOST_TEST((edt::rewrite_references(text, std::vector<std::string>{ "/sky", "/gone" }, "res://sky.desc", "res://b.desc", false).status
		== edt::rewrite_status::pointer_missing));
	BOOST_TEST((edt::rewrite_references(text, std::vector<std::string>{ "/count" }, "res://sky.desc", "res://b.desc", false).status
		== edt::rewrite_status::not_a_reference));
	BOOST_TEST((edt::rewrite_references(text, std::vector<std::string>{ "/other" }, "res://sky.desc", "res://b.desc", false).status
		== edt::rewrite_status::not_a_reference));
	BOOST_TEST((edt::rewrite_references("not json", std::vector<std::string>{ "/a" }, "res://sky.desc", "res://b.desc", false).status
		== edt::rewrite_status::unreadable));
}

BOOST_AUTO_TEST_CASE(ReferencesThatAlreadyHoldTheNewPath_NeedNoWrite)
{
	const std::string text = canonical(R"({ "sky": "res://b.desc" })");
	const edt::rewrite_outcome outcome = edt::rewrite_references(text, std::vector<std::string>{ "/sky" }, "res://b.desc", "res://b.desc", false);
	BOOST_TEST((outcome.status == edt::rewrite_status::nothing_to_do));
}

BOOST_AUTO_TEST_SUITE_END()
