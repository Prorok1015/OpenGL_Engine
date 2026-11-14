#include <boost/test/unit_test.hpp>
#include "edt_agent_log_filter.h"
#include <filesystem>
#include <string>

// #935: what log.read gives an agent of the engine's journal.
namespace {

edt::agent_log_roots roots()
{
	edt::agent_log_roots out;
	out.project = "/home/dev/games/demo";
	out.home = "/home/dev";
	out.temp = "/tmp";
	out.top_level = { "home", "tmp", "usr", "etc" };
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EdtAgentLogFilterTests)

BOOST_AUTO_TEST_CASE(SomeCategories_AreNeverAnAgents_TheBuildOnlyWhileItMayNotBuild)
{
	for (const char* hidden : { "edt/decoder", "capture", "mcp", "mcp/http", "editor/agent_server", "editor/agent_policy" }) {
		BOOST_TEST_CONTEXT(hidden) {
			BOOST_TEST(!edt::agent_log_category_visible(hidden, false));
			BOOST_TEST(!edt::agent_log_category_visible(hidden, true));
		}
	}
	BOOST_TEST(!edt::agent_log_category_visible("editor/build", false));
	BOOST_TEST(!edt::agent_log_category_visible("editor/build/cmake", false));
	BOOST_TEST(edt::agent_log_category_visible("editor/build", true));
	for (const char* shown : { "desc", "scene", "resource/write", "play", "editor", "editor/builder", "mcpx", "captures" }) {
		BOOST_TEST_CONTEXT(shown) {
			BOOST_TEST(edt::agent_log_category_visible(shown, false));
		}
	}
}

BOOST_AUTO_TEST_CASE(PathsOfTheProjectKeepTheirTail_TheOthersBecomeAnonymous)
{
	const edt::agent_log_roots known = roots();
	BOOST_TEST(edt::agent_log_text("loaded /home/dev/games/demo/res/levels/a.desc", known) == "loaded <project>/res/levels/a.desc");
	BOOST_TEST(edt::agent_log_text("token file '/home/dev/.config/snk/token' missing", known) == "token file '<home>/.config/snk/token' missing");
	BOOST_TEST(edt::agent_log_text("scratch /tmp/snk_decode_17/in.glb", known) == "scratch <tmp>/snk_decode_17/in.glb");
	BOOST_TEST(edt::agent_log_text("cmake at /usr/bin/cmake, config (/etc/snk.conf)", known) == "cmake at <path>, config (<path>)");
	BOOST_TEST(edt::agent_log_text(R"(wrote C:\Users\dev\secret.txt and \\server\share\x)", known) == "wrote <path> and <path>");
	BOOST_TEST(edt::agent_log_text("path=/usr/lib/x.so;next", known) == "path=<path>;next");
}

BOOST_AUTO_TEST_CASE(WhatOnlyLooksLikeAPath_IsLeftAlone)
{
	const edt::agent_log_roots known = roots();
	for (const char* text : { "res://levels/a.desc /header/type: unknown texture type", "ratio 3/4, either/or", "https://example.org/x",
		"a.desc /components/mesh/geometry: not found", "<project>/res/a.desc" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, known) == text);
		}
	}
}

BOOST_AUTO_TEST_CASE(WithoutRoots_OnlyAbsolutePathsGo)
{
	edt::agent_log_roots none;
	none.top_level = { "home" };
	BOOST_TEST(edt::agent_log_text("at /home/x/y and /header/type", none) == "at <path> and /header/type");
	BOOST_TEST(edt::agent_log_text("", none) == "");
}

BOOST_AUTO_TEST_SUITE_END()
