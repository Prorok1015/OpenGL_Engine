#include <boost/test/unit_test.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include "app_frame_stage.h"

namespace {

using names_t = std::vector<std::string>;

app::frame_stage_registry::stage_fn noop() { return [](app::stage_context&) {}; }

}

BOOST_AUTO_TEST_SUITE(AppFrameStageTests)

BOOST_AUTO_TEST_CASE(BuiltinStagesRunInCanonicalOrder) {
	app::frame_stage_registry registry;
	const names_t expected{ "begin", "input", "update", "post_update", "extract", "render", "ui", "present" };
	BOOST_TEST(registry.stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(BuiltinNamesRoundTrip) {
	for (std::size_t i = 0; i < app::builtin_stage_count; ++i) {
		const auto stage = static_cast<app::builtin_stage>(i);
		BOOST_TEST(app::parse_builtin_stage(app::to_string(stage)).has_value());
		BOOST_TEST((*app::parse_builtin_stage(app::to_string(stage)) == stage));
	}
	BOOST_TEST(!app::parse_builtin_stage("nope").has_value());
}

BOOST_AUTO_TEST_CASE(StagesRunBeforeAndAfterTheirAnchor) {
	app::frame_stage_registry registry;
	BOOST_REQUIRE(registry.add_stage("pre_render", app::builtin_stage::render, app::stage_side::before, noop()));
	BOOST_REQUIRE(registry.add_stage("post_render", app::builtin_stage::render, app::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("late", app::builtin_stage::present, app::stage_side::after, noop()));

	const names_t expected{ "begin", "input", "update", "post_update", "extract",
		"pre_render", "render", "post_render", "ui", "present", "late" };
	BOOST_TEST(registry.stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(SameAnchorKeepsRegistrationOrder) {
	app::frame_stage_registry registry;
	BOOST_REQUIRE(registry.add_stage("b", app::builtin_stage::update, app::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("a", app::builtin_stage::update, app::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("c", app::builtin_stage::update, app::stage_side::after, noop()));

	const names_t expected{ "begin", "input", "update", "b", "a", "c", "post_update", "extract", "render", "ui", "present" };
	BOOST_TEST(registry.stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(DuplicateAndBuiltinNamesAreRejected) {
	app::frame_stage_registry registry;
	BOOST_REQUIRE(registry.add_stage("mine", app::builtin_stage::update, app::stage_side::after, noop()));
	BOOST_TEST(!registry.add_stage("mine", app::builtin_stage::render, app::stage_side::before, noop()));
	BOOST_TEST(!registry.add_stage("render", app::builtin_stage::ui, app::stage_side::after, noop()));
	BOOST_TEST(!registry.add_stage("", app::builtin_stage::ui, app::stage_side::after, noop()));
	BOOST_TEST(registry.stage_names().size() == app::builtin_stage_count + 1);
}

BOOST_AUTO_TEST_CASE(BeginIsAlwaysFirst) {
	app::frame_stage_registry registry;
	BOOST_TEST(!registry.add_stage("too_early", app::builtin_stage::begin, app::stage_side::before, noop()));
	BOOST_REQUIRE(registry.add_stage("after_begin", app::builtin_stage::begin, app::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("before_input", app::builtin_stage::input, app::stage_side::before, noop()));

	const auto names = registry.stage_names();
	BOOST_REQUIRE(!names.empty());
	BOOST_TEST(names.front() == "begin");
	BOOST_TEST(names[1] == "after_begin");
}

BOOST_AUTO_TEST_CASE(BuiltinBodyCanBeSetOnlyOnce) {
	app::frame_stage_registry registry;
	BOOST_TEST(registry.set_body(app::builtin_stage::ui, noop()));
	BOOST_TEST(!registry.set_body(app::builtin_stage::ui, noop()));
}

BOOST_AUTO_TEST_CASE(RunExecutesBodiesAndStagesInOrder) {
	app::frame_stage_registry registry;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](app::stage_context&) { trace += c; };
	};
	registry.set_body(app::builtin_stage::begin, mark('B'));
	registry.set_body(app::builtin_stage::update, mark('U'));
	registry.set_body(app::builtin_stage::present, mark('P'));
	registry.add_stage("x", app::builtin_stage::update, app::stage_side::before, mark('x'));
	registry.add_stage("y", app::builtin_stage::update, app::stage_side::after, mark('y'));

	ds::app_data_storage storage;
	app::stage_context ctx{ storage, std::chrono::duration<float>{ 0.016f } };
	registry.run(ctx);
	BOOST_TEST(trace == "BxUyP");
}

BOOST_AUTO_TEST_CASE(StageMayAddAndRemoveStagesWhileRunning) {
	app::frame_stage_registry registry;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](app::stage_context&) { trace += c; };
	};
	registry.add_stage("late", app::builtin_stage::present, app::stage_side::after, mark('L'));
	registry.add_stage("self_modifying", app::builtin_stage::update, app::stage_side::after,
		[&](app::stage_context&) {
			trace += 'S';
			registry.add_stage("added_during_run", app::builtin_stage::render, app::stage_side::after, mark('A'));
			registry.remove_stage("late");
		});

	ds::app_data_storage storage;
	app::stage_context ctx{ storage, std::chrono::duration<float>{ 0.f } };
	registry.run(ctx);
	BOOST_TEST(trace == "SL"); // this frame's plan was fixed before it started
	trace.clear();
	registry.run(ctx);
	BOOST_TEST(trace == "SA"); // changes apply from the next frame
}

BOOST_AUTO_TEST_CASE(TermHooksAllRunEvenWhenOneThrows) {
	app::frame_stage_registry registry;
	std::string trace;
	registry.add_term_hook([&](ds::app_data_storage&) { trace += "1"; });
	registry.add_term_hook([&](ds::app_data_storage&) { trace += "2"; throw std::runtime_error("boom"); });
	registry.add_term_hook([&](ds::app_data_storage&) { trace += "3"; });

	ds::app_data_storage storage;
	BOOST_CHECK_THROW(registry.run_term_hooks(storage), std::runtime_error);
	BOOST_TEST(trace == "321");
}

BOOST_AUTO_TEST_SUITE_END()
