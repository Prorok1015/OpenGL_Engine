#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <limits>
#include <vector>

BOOST_AUTO_TEST_SUITE(EcsFixedStepperTests)

BOOST_AUTO_TEST_CASE(SameDeltaSequence_GivesTheSameStepSequence)
{
	const std::vector<double> deltas = { 0.04, 0.04, 0.04, 0.001, 0.016, 0.0, 0.1 };
	auto run = [&] {
		ecs::fixed_stepper stepper;
		std::vector<int> steps;
		for (double dt : deltas) steps.push_back(stepper.advance(dt));
		return steps;
	};

	BOOST_TEST(run() == run(), boost::test_tools::per_element());
	BOOST_TEST(run() == (std::vector<int>{ 2, 2, 3, 0, 1, 0, 6 }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(SmallDeltas_AccumulateIntoWholeSteps)
{
	ecs::fixed_stepper stepper(1.0 / 16.0); // binary-exact values keep the sums exact
	int total = 0;
	for (int i = 0; i < 16; ++i) total += stepper.advance(1.0 / 64.0);
	BOOST_TEST(total == 4);
}

BOOST_AUTO_TEST_CASE(HugeDelta_IsClampedAndStepsAreCapped)
{
	ecs::fixed_stepper stepper(1.0 / 60.0, 0.25, 8);
	BOOST_TEST(stepper.advance(10.0) == 8);
	// the backlog beyond the cap is dropped: the next quiet frame runs nothing
	BOOST_TEST(stepper.advance(0.0) == 0);
	BOOST_TEST(stepper.alpha() < 1.0f);
}

BOOST_AUTO_TEST_CASE(NegativeAndNaN_AreTreatedAsZero)
{
	ecs::fixed_stepper stepper;
	BOOST_TEST(stepper.advance(-1.0) == 0);
	BOOST_TEST(stepper.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
	BOOST_TEST(stepper.clamp_dt(-5.0) == 0.0);
}

BOOST_AUTO_TEST_CASE(Alpha_IsTheFractionOfTheNextStep)
{
	ecs::fixed_stepper stepper;
	stepper.advance(1.5 / 60.0);
	BOOST_TEST(stepper.alpha() == 0.5f, boost::test_tools::tolerance(1e-4));
}

BOOST_AUTO_TEST_SUITE_END()
