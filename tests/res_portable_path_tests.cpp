#include <boost/test/unit_test.hpp>
#include <snk/resource/res_path_sanitize.h>

// Which relative paths every platform can hold (res::portable_path_problem, used by mounts that write code, #450).
BOOST_AUTO_TEST_SUITE(ResPortablePath)

BOOST_AUTO_TEST_CASE(PlainNames_AreFine)
{
	BOOST_TEST(res::portable_path_problem("a.cpp").empty());
	BOOST_TEST(res::portable_path_problem("src/game/player.hpp").empty());
	BOOST_TEST(res::portable_path_problem("CMakeLists.txt").empty());
	BOOST_TEST(res::portable_path_problem("\xE6\x97\xA5\xE6\x9C\xAC/\xE6\x9C\xAC.cpp").empty());
	BOOST_TEST(res::portable_path_problem("console.cpp").empty());
	BOOST_TEST(res::portable_path_problem("nullable.h").empty());
	BOOST_TEST(res::portable_path_problem("com10.cpp").empty());
	BOOST_TEST(res::portable_path_problem("lpt0.cpp").empty());
}

BOOST_AUTO_TEST_CASE(StreamsAndDrives_AreRefused)
{
	for (const char* bad : { "a:b.cpp", "build:x.cpp", "CMakeLists.txt:a.cpp", "src/a.cpp:stream", "c:x", "src/a:b/c.cpp" }) {
		BOOST_TEST_INFO(bad);
		BOOST_TEST(!res::portable_path_problem(bad).empty());
	}
}

BOOST_AUTO_TEST_CASE(DeviceNames_AreRefusedWithAnyExtensionAndCase)
{
	for (const char* bad : { "con", "CON.cpp", "src/Prn.h", "aux.txt", "NUL", "nul.tar.gz", "com1.cpp", "COM9", "lpt1.h", "LPT9.cpp",
	                         "conin$", "CONOUT$.cpp", "com\xC2\xB9.cpp", "LPT\xC2\xB2", "com\xC2\xB3", "con .cpp" }) {
		BOOST_TEST_INFO(bad);
		BOOST_TEST(!res::portable_path_problem(bad).empty());
	}
}

BOOST_AUTO_TEST_CASE(TrailingDotsSpacesAndControlCharacters_AreRefused)
{
	for (const char* bad : { "a.cpp.", "a.cpp ", "src /a.cpp", "src./a.cpp", "a\x01.cpp", "a\x7f.cpp", "", "a//b.cpp", "./a.cpp", "../a.cpp", "a/..", "a/" }) {
		BOOST_TEST_INFO(bad);
		BOOST_TEST(!res::portable_path_problem(bad).empty());
	}
}

BOOST_AUTO_TEST_SUITE_END()
