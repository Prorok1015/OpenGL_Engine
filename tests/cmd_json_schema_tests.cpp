#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/parse.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
	boost::json::value json_of(std::string_view text)
	{
		return boost::json::parse(text);
	}
}

BOOST_AUTO_TEST_SUITE(CmdJsonSchemaTests)

BOOST_AUTO_TEST_CASE(NoArgsSchema_AcceptsOnlyAnEmptyObject)
{
	const auto schema = cmd::no_args_schema();
	BOOST_CHECK(!cmd::json_schema::check_schema(schema));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of("{}")));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"x":1})")));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of("[]")));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of("null")));
}

BOOST_AUTO_TEST_CASE(Validate_ChecksTypeRequiredEnumAndRange)
{
	const auto schema = json_of(R"({"type":"object","properties":{
		"name":{"type":"string"},"mode":{"enum":["a","b"]},"count":{"type":"integer","minimum":1,"maximum":5},
		"flag":{"type":["boolean","null"]}},"required":["name"],"additionalProperties":false})");
	BOOST_REQUIRE(!cmd::json_schema::check_schema(schema));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of(R"({"name":"x","mode":"a","count":3,"flag":null})")));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({})")));                           // required
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":1})")));                   // type
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":"x","mode":"c"})")));      // enum
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":"x","count":0})")));       // minimum
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":"x","count":6})")));       // maximum
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":"x","count":1.5})")));     // integer
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":"x","flag":"yes"})")));    // type list
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"({"name":"x","other":1})")));       // additionalProperties
}

BOOST_AUTO_TEST_CASE(MinLength_CountsCharacters)
{
	const auto schema = json_of(R"({"type":"string","minLength":2})");
	BOOST_REQUIRE(!cmd::json_schema::check_schema(schema));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"("")")));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"("a")")));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of(R"("ab")")));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of("\"\xC3\xA9\xC3\xA9\""))); // two characters, four bytes
	BOOST_CHECK(cmd::json_schema::check_schema(json_of(R"({"minLength":-1})")).has_value());
	BOOST_CHECK(cmd::json_schema::check_schema(json_of(R"({"minLength":"1"})")).has_value());
}

BOOST_AUTO_TEST_CASE(CheckSchema_RefusesKeywordsItDoesNotImplement)
{
	for (const char* text : { R"({"pattern":"a"})", R"({"uniqueItems":true})", R"({"maxLength":3})",
		R"({"oneOf":[]})", R"({"$ref":"#/x"})", R"({"default":1})",
		R"({"type":"object","properties":{"a":{"format":"uri"}}})" }) {
		BOOST_TEST(cmd::json_schema::check_schema(json_of(text)).has_value(), text);
	}
	BOOST_CHECK(!cmd::json_schema::check_schema(json_of(R"({"type":"string","description":"d","title":"t"})")));
}

BOOST_AUTO_TEST_CASE(AdditionalProperties_AreAllowedUnlessForbidden)
{
	const auto schema = json_of(R"({"type":"object","properties":{"a":{"type":"string"}}})");
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of(R"({"a":"x","b":2})")));
}

BOOST_AUTO_TEST_CASE(CheckSchema_RejectsBrokenSchemas)
{
	for (const char* text : { "1", R"({"type":"thing"})", R"({"type":[]})", R"({"properties":3})",
		R"({"required":["a"]})", R"({"required":[1],"properties":{"a":{}}})", R"({"additionalProperties":"no"})",
		R"({"enum":[]})", R"({"minimum":"1"})", R"({"properties":{"a":{"type":"x"}}})",
		R"({"items":3})", R"({"items":{"type":"x"}})", R"({"minProperties":-1})", R"({"minProperties":"1"})" }) {
		BOOST_TEST(cmd::json_schema::check_schema(json_of(text)).has_value(), text);
	}
}

BOOST_AUTO_TEST_CASE(Items_ValidatesEveryElementAndNamesTheOffender)
{
	const auto schema = json_of(R"({"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"}},
		"required":["id"],"additionalProperties":false}})");
	BOOST_REQUIRE(!cmd::json_schema::check_schema(schema));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of("[]")));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of(R"([{"id":1},{"id":2}])")));
	const auto bad_type = cmd::json_schema::validate(schema, json_of(R"([{"id":1},{"id":"x"}])"));
	BOOST_REQUIRE(bad_type.has_value());
	BOOST_CHECK(bad_type->find("[1]") != std::string::npos);
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"([{"id":1,"extra":true}])")));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of(R"([{}])")));
}

BOOST_AUTO_TEST_CASE(MinProperties_NeedsAtLeastThatManyKeys)
{
	const auto schema = json_of(R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"boolean"}},
		"minProperties":1,"additionalProperties":false})");
	BOOST_REQUIRE(!cmd::json_schema::check_schema(schema));
	BOOST_CHECK(cmd::json_schema::validate(schema, json_of("{}")));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of(R"({"a":"x"})")));
	BOOST_CHECK(!cmd::json_schema::validate(schema, json_of(R"({"b":true})")));
}

BOOST_AUTO_TEST_CASE(IntegerOf_ReadsInt64Uint64AndIntegralDoubles)
{
	using cmd::json_schema::integer_of;
	BOOST_CHECK_EQUAL(*integer_of(json_of("5")), 5);
	BOOST_CHECK_EQUAL(*integer_of(json_of("-7")), -7);
	BOOST_CHECK_EQUAL(*integer_of(json_of("5.0")), 5);
	BOOST_CHECK_EQUAL(*integer_of(json_of("1e3")), 1000);
	BOOST_CHECK_EQUAL(*integer_of(boost::json::value(std::uint64_t{ 9 })), 9);
	// What the schema type "integer" accepts but no int64 holds, and what it rejects, gives nothing.
	BOOST_CHECK(!integer_of(json_of("18446744073709551615")));
	BOOST_CHECK(!integer_of(json_of("1e30")));
	BOOST_CHECK(!integer_of(json_of("5.5")));
	BOOST_CHECK(!integer_of(json_of(R"("5")")));
	BOOST_CHECK(!integer_of(json_of("null")));
	BOOST_CHECK(!integer_of(json_of("true")));
}

BOOST_AUTO_TEST_CASE(IntegerSchema_AcceptsExactlyWhatIntegerOfReads)
{
	const auto schema = json_of(R"({"type":"integer"})");
	for (const char* text : { "5", "5.0", "-3", "1e3" }) {
		BOOST_TEST(!cmd::json_schema::validate(schema, json_of(text)), text);
		BOOST_TEST(cmd::json_schema::integer_of(json_of(text)).has_value(), text);
	}
	for (const char* text : { "5.5", R"("5")", "null" }) {
		BOOST_TEST(cmd::json_schema::validate(schema, json_of(text)).has_value(), text);
		BOOST_TEST(!cmd::json_schema::integer_of(json_of(text)).has_value(), text);
	}
}

BOOST_AUTO_TEST_SUITE_END()
