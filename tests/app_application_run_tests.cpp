#include <boost/test/unit_test.hpp>
#include <stdexcept>
#include <string>
#include "application.h"
#include "app_loop_service_interface.h"

namespace {

struct recording_loop : app::app_loop_service_interface
{
	explicit recording_loop(int steps, int throw_at = -1) : m_steps(steps), m_throw_at(throw_at) {}

	void init(ds::app_data_storage&) override { trace += "I"; }
	void on_step(ds::app_data_storage&) override
	{
		if (done == m_throw_at) {
			trace += "!";
			throw std::runtime_error("step failed");
		}
		trace += "S";
		++done;
	}
	void term(ds::app_data_storage&) override { trace += "T"; }
	bool should_stop() const override { return done >= m_steps; }
	void request_stop() override { m_steps = 0; }

	std::string trace;
	int done = 0;
	int m_steps;
	int m_throw_at;
};

}

BOOST_AUTO_TEST_SUITE(AppApplicationRunTests)

BOOST_AUTO_TEST_CASE(RunsInitStepsThenTerm) {
	ds::app_data_storage storage;
	auto& loop = storage.construct<app::app_loop_service_interface, recording_loop>(3);
	app::application app;
	BOOST_TEST(app.run(storage) == 0);
	BOOST_TEST(static_cast<recording_loop&>(loop).trace == "ISSST");
}

BOOST_AUTO_TEST_CASE(ZeroStepsStillInitAndTerm) {
	ds::app_data_storage storage;
	auto& loop = storage.construct<app::app_loop_service_interface, recording_loop>(0);
	app::application app;
	BOOST_TEST(app.run(storage) == 0);
	BOOST_TEST(static_cast<recording_loop&>(loop).trace == "IT");
}

BOOST_AUTO_TEST_CASE(TermRunsAfterThrowingStepAndExceptionPropagates) {
	ds::app_data_storage storage;
	auto& loop = storage.construct<app::app_loop_service_interface, recording_loop>(5, 2);
	app::application app;
	BOOST_CHECK_THROW(app.run(storage), std::runtime_error);
	BOOST_TEST(static_cast<recording_loop&>(loop).trace == "ISS!T");
}

BOOST_AUTO_TEST_CASE(RunWithoutLoopReturnsMinusOne) {
	ds::app_data_storage storage;
	app::application app;
	BOOST_TEST(app.run(storage) == -1);
}

BOOST_AUTO_TEST_SUITE_END()
