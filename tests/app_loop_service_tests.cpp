#include <boost/test/unit_test.hpp>
#include <string>
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>

BOOST_AUTO_TEST_SUITE(AppLoopServiceTests)

BOOST_AUTO_TEST_CASE(StepRunsRegistryStagesInOrder) {
	app::frame_stage_registry stages;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](app::stage_context&) { trace += c; };
	};
	stages.set_body(app::builtin_stage::update, mark('U'));
	stages.add_stage("before_update", app::builtin_stage::update, app::stage_side::before, mark('b'));
	stages.add_stage("after_update", app::builtin_stage::update, app::stage_side::after, mark('a'));

	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.init(storage);
	loop.on_step(storage);
	loop.on_step(storage);
	BOOST_TEST(trace == "bUabUa");
}

BOOST_AUTO_TEST_CASE(StageSeesStorageAndPositiveDelta) {
	struct counter { int value = 0; };
	app::frame_stage_registry stages;
	float seen_dt = -1.f;
	stages.set_body(app::builtin_stage::update, [&](app::stage_context& c) {
		c.storage.require<counter>().value++;
		seen_dt = c.dt.count();
	});

	ds::app_data_storage storage;
	storage.construct<counter>();
	app::loop_service loop(stages);
	loop.init(storage);
	loop.on_step(storage);
	BOOST_TEST(storage.require<counter>().value == 1);
	BOOST_TEST(seen_dt >= 0.f);
}

BOOST_AUTO_TEST_CASE(RequestStopEndsLoop) {
	app::frame_stage_registry stages;
	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.init(storage);
	BOOST_TEST(!loop.should_stop());
	loop.request_stop();
	BOOST_TEST(loop.should_stop());
}

BOOST_AUTO_TEST_CASE(InitAndTermHooksRunInOrder) {
	app::frame_stage_registry stages;
	std::string trace;
	stages.add_init_hook([&](ds::app_data_storage&) { trace += "i1"; });
	stages.add_init_hook([&](ds::app_data_storage&) { trace += "i2"; });
	stages.add_term_hook([&](ds::app_data_storage&) { trace += "t1"; });
	stages.add_term_hook([&](ds::app_data_storage&) { trace += "t2"; });

	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.init(storage);
	loop.term(storage);
	BOOST_TEST(trace == "i1i2t2t1");
}

BOOST_AUTO_TEST_CASE(RemovedStageAndClearedBodyNoLongerRun) {
	app::frame_stage_registry stages;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](app::stage_context&) { trace += c; };
	};
	stages.set_body(app::builtin_stage::ui, mark('u'));
	stages.add_stage("extra", app::builtin_stage::ui, app::stage_side::after, mark('x'));
	BOOST_TEST(!stages.remove_stage("ui"));
	BOOST_TEST(stages.remove_stage("extra"));
	BOOST_TEST(!stages.remove_stage("extra"));
	stages.clear_body(app::builtin_stage::ui);

	ds::app_data_storage storage;
	app::stage_context ctx{ storage, std::chrono::duration<float>{ 0.f } };
	stages.run(ctx);
	BOOST_TEST(trace.empty());
	BOOST_TEST(stages.set_body(app::builtin_stage::ui, mark('u')));
}

BOOST_AUTO_TEST_SUITE_END()
