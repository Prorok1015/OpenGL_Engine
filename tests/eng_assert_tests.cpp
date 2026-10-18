#include <boost/test/unit_test.hpp>
#include <snk/common/engine_assert.h>

BOOST_AUTO_TEST_SUITE(EngineAssertTests)

BOOST_AUTO_TEST_CASE(VerifyEvaluatesConditionExactlyOnce)
{
	int calls = 0;
	const auto cond_true = [&] { ++calls; return true; };
	const auto cond_false = [&] { ++calls; return false; };

	BOOST_CHECK(ENGINE_VERIFY(cond_true(), "should not fire"));
	BOOST_CHECK_EQUAL(calls, 1);

	BOOST_CHECK(!ENGINE_VERIFY(cond_false(), "expected failure with arg {}", 7));
	BOOST_CHECK_EQUAL(calls, 2);
}

BOOST_AUTO_TEST_CASE(VerifyAcceptsPointersAndSmartPointers)
{
	int value = 0;
	int* null_ptr = nullptr;
	auto sp = std::make_shared<int>(1);
	BOOST_CHECK(ENGINE_VERIFY(&value, "ptr"));
	BOOST_CHECK(!ENGINE_VERIFY(null_ptr, "null ptr (expected log)"));
	BOOST_CHECK(ENGINE_VERIFY(sp, "shared"));
}

BOOST_AUTO_TEST_CASE(PassingAssertDoesNotAbort)
{
	ASSERT_MSG(1 + 1 == 2, "arithmetic broke");
	ASSERT_MSG(true, "formatted {} {}", 1, "two");
	BOOST_CHECK(true);
}

#if SNK_ASSERTS_ENABLED
BOOST_AUTO_TEST_CASE(AssertConditionEvaluatedWhenEnabled)
{
	int calls = 0;
	ASSERT_MSG(++calls == 1, "side effect");
	BOOST_CHECK_EQUAL(calls, 1);
}
#else
BOOST_AUTO_TEST_CASE(AssertConditionNotEvaluatedWhenDisabled)
{
	int calls = 0;
	ASSERT_MSG(++calls == 1, "side effect");
	BOOST_CHECK_EQUAL(calls, 0);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
