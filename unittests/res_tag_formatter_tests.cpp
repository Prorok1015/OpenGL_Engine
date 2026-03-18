#include <boost/test/unit_test.hpp>
#include "res_tag.h"
#include <format>
#include <sstream>

BOOST_AUTO_TEST_SUITE(ResTagFormatterTests)

static const auto TAG = res::tag("res://models/character/hero.glb");

// --- std::format default ---

BOOST_AUTO_TEST_CASE(Format_Default_FullString) {
	BOOST_TEST(std::format("{}", TAG) == "res://models/character/hero.glb");
}

// --- std::format specifiers ---

BOOST_AUTO_TEST_CASE(Format_Name) {
	BOOST_TEST(std::format("{:name}", TAG) == "hero.glb");
}

BOOST_AUTO_TEST_CASE(Format_Path) {
	BOOST_TEST(std::format("{:path}", TAG) == "models/character/");
}

BOOST_AUTO_TEST_CASE(Format_Extension) {
	BOOST_TEST(std::format("{:ext}", TAG) == "glb");
}

BOOST_AUTO_TEST_CASE(Format_PureName) {
	BOOST_TEST(std::format("{:pure}", TAG) == "hero");
}

BOOST_AUTO_TEST_CASE(Format_Protocol) {
	BOOST_TEST(std::format("{:proto}", TAG) == "res");
}

BOOST_AUTO_TEST_CASE(Format_Relative) {
	BOOST_TEST(std::format("{:rel}", TAG) == "models/character/hero.glb");
}

// --- std::format with surrounding text ---

BOOST_AUTO_TEST_CASE(Format_InContext) {
	auto result = std::format("Loading {:name} from {:proto}://", TAG, TAG);
	BOOST_TEST(result == "Loading hero.glb from res://");
}

// --- std::format unknown spec falls back to full ---

BOOST_AUTO_TEST_CASE(Format_UnknownSpec_FallsBackToFull) {
	BOOST_TEST(std::format("{:unknown}", TAG) == "res://models/character/hero.glb");
}

// --- ostream operator<< ---

BOOST_AUTO_TEST_CASE(OStream_OutputsFullString) {
	std::ostringstream oss;
	oss << TAG;
	BOOST_TEST(oss.str() == "res://models/character/hero.glb");
}

// --- memory protocol ---

BOOST_AUTO_TEST_CASE(Format_MemoryProtocol) {
	auto t = res::tag("memory", "runtime/mesh.geom");
	BOOST_TEST(std::format("{:proto}", t) == "memory");
	BOOST_TEST(std::format("{:name}", t) == "mesh.geom");
}

// --- invalid/empty tag ---

BOOST_AUTO_TEST_CASE(Format_EmptyTag) {
	res::tag empty;
	BOOST_TEST(std::format("{}", empty) == "");
}

// --- Boost.Test printability (operator<< enables BOOST_TEST with ==) ---

BOOST_AUTO_TEST_CASE(BoostTest_Printable) {
	auto a = res::tag::make("test/file.txt");
	auto b = res::tag::make("test/file.txt");
	BOOST_TEST(a == b);
}

BOOST_AUTO_TEST_SUITE_END()
