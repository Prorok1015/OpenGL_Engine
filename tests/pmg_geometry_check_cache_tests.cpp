#include <boost/test/unit_test.hpp>
#include <snk/project_migration/pmg_geometry_check_cache.h>
#include <string>

namespace
{
	struct counted_check
	{
		std::string answer;
		int calls = 0;

		std::string operator()()
		{
			++calls;
			return answer;
		}
	};

	std::string ask(prj::migration::geometry_check_cache& cache, std::string_view path, std::string_view text, counted_check& check)
	{
		return cache.problem(path, text, [&] { return check(); });
	}
}

BOOST_AUTO_TEST_SUITE(PmgGeometryCheckCacheTests)

BOOST_AUTO_TEST_CASE(TheSameTextAtTheSamePath_IsCheckedOnce)
{
	prj::migration::geometry_check_cache cache;
	counted_check check{ "the stored bounds are not those the loader builds" };
	BOOST_TEST(ask(cache, "res/a.desc", R"({"__type":"geometry_desc"})", check) == check.answer);
	BOOST_TEST(ask(cache, "res/a.desc", R"({"__type":"geometry_desc"})", check) == check.answer);
	BOOST_TEST(check.calls == 1);
}

BOOST_AUTO_TEST_CASE(AnEmptyAnswer_IsRememberedToo)
{
	prj::migration::geometry_check_cache cache;
	counted_check check{ "" };
	BOOST_TEST(ask(cache, "res/a.desc", "{}", check).empty());
	BOOST_TEST(ask(cache, "res/a.desc", "{}", check).empty());
	BOOST_TEST(check.calls == 1);
}

BOOST_AUTO_TEST_CASE(AChangedText_IsCheckedAgain_EvenOfTheSameSize)
{
	prj::migration::geometry_check_cache cache;
	counted_check check{ "" };
	ask(cache, "res/a.desc", R"({"v":[1,2]})", check);
	ask(cache, "res/a.desc", R"({"v":[1,3]})", check);
	ask(cache, "res/a.desc", R"({"v":[1,2,4]})", check);
	BOOST_TEST(check.calls == 3);
}

BOOST_AUTO_TEST_CASE(TheSameTextAtAnotherPath_IsCheckedAgain)
{
	prj::migration::geometry_check_cache cache;
	counted_check check{ "" };
	ask(cache, "res/a.desc", "{}", check);
	ask(cache, "res/b.desc", "{}", check);
	BOOST_TEST(check.calls == 2);
}

BOOST_AUTO_TEST_CASE(AFullCache_StartsOver)
{
	prj::migration::geometry_check_cache cache(2);
	counted_check check{ "" };
	ask(cache, "res/a.desc", "{}", check);
	ask(cache, "res/b.desc", "{}", check);
	BOOST_TEST(cache.size() == 2u);
	ask(cache, "res/c.desc", "{}", check);
	BOOST_TEST(cache.size() == 1u);
	ask(cache, "res/a.desc", "{}", check);
	BOOST_TEST(check.calls == 4);
}

BOOST_AUTO_TEST_CASE(Clear_ForgetsEveryAnswer)
{
	prj::migration::geometry_check_cache cache;
	counted_check check{ "" };
	ask(cache, "res/a.desc", "{}", check);
	cache.clear();
	BOOST_TEST(cache.size() == 0u);
	ask(cache, "res/a.desc", "{}", check);
	BOOST_TEST(check.calls == 2);
}

BOOST_AUTO_TEST_SUITE_END()
