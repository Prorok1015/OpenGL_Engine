#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_entry.h>
#include <snk/game/gs_module_layout.h>
#include <cstdint>
#include <string>

// #614: the layout key a module reports is evaluated from the headers it was built with; the host compares it with the
// engine's own before it creates the module.

namespace {

std::uint64_t g_reported = 0;
std::uint64_t reported_key() { return g_reported; }

app::modules::module_entry entry_reporting(std::uint64_t key)
{
	g_reported = key;
	app::modules::module_entry entry;
	entry.layout_key = &reported_key;
	return entry;
}

}

BOOST_AUTO_TEST_SUITE(gs_module_layout_tests)

// Every CI configuration (Debug, Release, sanitizers, MSVC) runs this: a binary built with these headers and the engine
// of the same build agree, so a module rebuilt with the project loads.
BOOST_AUTO_TEST_CASE(a_binary_built_with_the_engine_headers_has_the_engine_key)
{
	static_assert(gs::module_layout_key() != 0u);
	BOOST_TEST(gs::module_layout_key() == gs::engine_module_layout_key());
	BOOST_TEST(SNK_DETAIL_MODULE_LAYOUT_KEY == gs::engine_module_layout_key());
}

BOOST_AUTO_TEST_CASE(a_size_or_alignment_change_of_a_listed_type_changes_the_key)
{
	struct grown { std::uint64_t a = 0; std::uint64_t b = 0; };
	BOOST_TEST(gs::detail::layout_of<std::uint64_t>() != gs::detail::layout_of<grown>());
	BOOST_TEST(gs::detail::layout_of<std::uint32_t, std::uint64_t>() != gs::detail::layout_of<std::uint64_t, std::uint32_t>());
	BOOST_TEST(gs::detail::layout_of<std::uint64_t>() == gs::detail::layout_of<std::uint64_t>());
}

BOOST_AUTO_TEST_CASE(the_host_refuses_another_key_and_lets_the_same_or_an_unknown_one_through)
{
	const std::uint64_t host = gs::engine_module_layout_key();
	BOOST_TEST(app::modules::check_module_layout(entry_reporting(host), host).empty());
	BOOST_TEST(app::modules::check_module_layout(entry_reporting(0u), host).empty());
	BOOST_TEST(app::modules::check_module_layout(entry_reporting(host ^ 1u), 0u).empty());
	const std::string why = app::modules::check_module_layout(entry_reporting(host ^ 1u), host);
	BOOST_TEST(why.find("another layout") != std::string::npos);
	BOOST_TEST(why.find("rebuild the project") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
