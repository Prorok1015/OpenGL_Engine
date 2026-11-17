#include <boost/test/unit_test.hpp>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_tag.h>
#include "test_temp_dir.hpp"

BOOST_AUTO_TEST_SUITE(ResPathSanitizeTests)

BOOST_AUTO_TEST_CASE(KeepsSafeRelativePaths)
{
	BOOST_TEST(res::sanitize_relative_path("x.png") == "x.png");
	BOOST_TEST(res::sanitize_relative_path("tex/x.png") == "tex/x.png");
	BOOST_TEST(res::sanitize_relative_path("tex\\sub\\x.png") == "tex/sub/x.png");
	BOOST_TEST(res::sanitize_relative_path("./tex//x.png") == "tex/x.png");
}

BOOST_AUTO_TEST_CASE(DropsParentRootAndDriveParts)
{
	BOOST_TEST(res::sanitize_relative_path("../../x.png") == "x.png");
	BOOST_TEST(res::sanitize_relative_path("a/../../x.png") == "a/x.png");
	BOOST_TEST(res::sanitize_relative_path("/abs/x.png") == "abs/x.png");
	BOOST_TEST(res::sanitize_relative_path("C:\\x.png") == "x.png");
	BOOST_TEST(res::sanitize_relative_path("C:/dir/x.png") == "dir/x.png");
	BOOST_TEST(res::sanitize_relative_path("\\\\server\\share\\x.png") == "server/share/x.png");
}

BOOST_AUTO_TEST_CASE(NeverProducesProtocolOrEmptyResult)
{
	BOOST_TEST(res::sanitize_relative_path("http://host/x.png") == "http_/host/x.png");
	BOOST_TEST(res::sanitize_relative_path("").size() > 0u);
	BOOST_TEST(res::sanitize_relative_path("..") == res::sanitize_relative_path(""));
	BOOST_TEST(res::sanitize_relative_path("../..\\") == res::sanitize_relative_path(""));
	BOOST_TEST(res::sanitize_relative_path(std::string("a\0b\nc", 5)) == "a_b_c");
}

// A material name such as "evil://x" used to reach the tag constructor and hit ASSERT_MSG("Path already has protocol!").
BOOST_AUTO_TEST_CASE(SanitizedNameIsAcceptedByTagConstructor)
{
	for (const char* name : { "evil://x", "a://b://c", "http://host/x", "::", "C:" }) {
		BOOST_TEST_CONTEXT("name " << name) {
			const res::tag t{ res::tag::memory, "model/materials/" + res::sanitize_relative_path(name) + ".mat.desc" };
			BOOST_TEST(t.string().find("://", 9) == std::string::npos);
		}
	}
}

BOOST_AUTO_TEST_CASE(CapsPartAndTotalLength)
{
	const std::string long_part(600, 'a');
	const auto one = res::sanitize_relative_path(long_part);
	BOOST_TEST(one.size() == res::max_sanitized_part_length);

	std::string many;
	for (int i = 0; i < 20; ++i) {
		many += std::string(200, 'b') + "/";
	}
	const auto total = res::sanitize_relative_path(many);
	BOOST_TEST(total.size() <= res::max_sanitized_path_length);
	BOOST_TEST(total.back() != '/');
}

struct model_dir_fixture
{
	test_util::temp_dir dir{ "path_within_dir" };
	std::filesystem::path model = dir.path() / "model";

	model_dir_fixture()
	{
		dir.write("model/ok.png", "png-bytes");
		dir.write("model/tex/sub.png", "sub-bytes");
		dir.write("secret.txt", "secret");
	}
};

BOOST_FIXTURE_TEST_CASE(ReadFileWithinDir_ReadsSiblingsAndSubdirs, model_dir_fixture)
{
	const auto a = res::read_file_within_dir(model, "ok.png");
	BOOST_REQUIRE(a.has_value());
	BOOST_TEST(a->size() == 9u);
	BOOST_TEST(res::read_file_within_dir(model, "tex/sub.png").has_value());
	BOOST_TEST(res::read_file_within_dir(model, "tex\\sub.png").has_value());
}

BOOST_FIXTURE_TEST_CASE(ReadFileWithinDir_SkipsEscapingAndAbsolutePaths, model_dir_fixture)
{
	BOOST_TEST(!res::read_file_within_dir(model, "../secret.txt").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "..\\secret.txt").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "tex/../../secret.txt").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, (dir.path() / "secret.txt").string()).has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "/etc/passwd").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "C:/secret.txt").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, std::string("ok.png\0x", 8)).has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "").has_value());
}

BOOST_FIXTURE_TEST_CASE(ReadFileWithinDir_SkipsDirectoriesAndOversizedFiles, model_dir_fixture)
{
	BOOST_TEST(!res::read_file_within_dir(model, "tex").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "missing.png").has_value());
	BOOST_TEST(!res::read_file_within_dir(model, "ok.png", 4).has_value());
	BOOST_TEST(res::read_file_within_dir(model, "ok.png", 9).has_value());
}

BOOST_FIXTURE_TEST_CASE(PathWithinDir_AbsoluteOnlyWhenAllowedAndInside, model_dir_fixture)
{
	BOOST_TEST(res::path_within_dir(model, (model / "ok.png").string(), true).has_value());
	BOOST_TEST(!res::path_within_dir(model, (model / "ok.png").string(), false).has_value());
	BOOST_TEST(!res::path_within_dir(model, (dir.path() / "secret.txt").string(), true).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
