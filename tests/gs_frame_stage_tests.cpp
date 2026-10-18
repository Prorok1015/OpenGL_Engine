#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>
#include "gs_frame_stage.h"
#include "rnd_frame_context.h"

namespace {

using names_t = std::vector<std::string>;

gs::frame_stage_registry::stage_fn noop() { return [](gs::frame_step_context&) {}; }

}

BOOST_AUTO_TEST_SUITE(GsFrameStageTests)

BOOST_AUTO_TEST_CASE(BuiltinStagesRunInCanonicalOrder) {
	gs::frame_stage_registry registry;
	const names_t expected{ "begin", "input", "update", "post_update", "extract", "render", "ui", "present" };
	BOOST_TEST(registry.stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(BuiltinNamesRoundTrip) {
	for (std::size_t i = 0; i < gs::builtin_stage_count; ++i) {
		const auto stage = static_cast<gs::builtin_stage>(i);
		BOOST_TEST(gs::parse_builtin_stage(gs::to_string(stage)).has_value());
		BOOST_TEST((*gs::parse_builtin_stage(gs::to_string(stage)) == stage));
	}
	BOOST_TEST(!gs::parse_builtin_stage("nope").has_value());
}

BOOST_AUTO_TEST_CASE(StagesRunBeforeAndAfterTheirAnchor) {
	gs::frame_stage_registry registry;
	BOOST_REQUIRE(registry.add_stage("pre_render", gs::builtin_stage::render, gs::stage_side::before, noop()));
	BOOST_REQUIRE(registry.add_stage("post_render", gs::builtin_stage::render, gs::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("late", gs::builtin_stage::present, gs::stage_side::after, noop()));

	const names_t expected{ "begin", "input", "update", "post_update", "extract",
		"pre_render", "render", "post_render", "ui", "present", "late" };
	BOOST_TEST(registry.stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(SameAnchorKeepsRegistrationOrder) {
	gs::frame_stage_registry registry;
	BOOST_REQUIRE(registry.add_stage("b", gs::builtin_stage::update, gs::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("a", gs::builtin_stage::update, gs::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("c", gs::builtin_stage::update, gs::stage_side::after, noop()));

	const names_t expected{ "begin", "input", "update", "b", "a", "c", "post_update", "extract", "render", "ui", "present" };
	BOOST_TEST(registry.stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(DuplicateAndBuiltinNamesAreRejected) {
	gs::frame_stage_registry registry;
	BOOST_REQUIRE(registry.add_stage("mine", gs::builtin_stage::update, gs::stage_side::after, noop()));
	BOOST_TEST(!registry.add_stage("mine", gs::builtin_stage::render, gs::stage_side::before, noop()));
	BOOST_TEST(!registry.add_stage("render", gs::builtin_stage::ui, gs::stage_side::after, noop()));
	BOOST_TEST(!registry.add_stage("", gs::builtin_stage::ui, gs::stage_side::after, noop()));
	BOOST_TEST(registry.stage_names().size() == gs::builtin_stage_count + 1);
}

BOOST_AUTO_TEST_CASE(BeginIsAlwaysFirst) {
	gs::frame_stage_registry registry;
	BOOST_TEST(!registry.add_stage("too_early", gs::builtin_stage::begin, gs::stage_side::before, noop()));
	BOOST_REQUIRE(registry.add_stage("after_begin", gs::builtin_stage::begin, gs::stage_side::after, noop()));
	BOOST_REQUIRE(registry.add_stage("before_input", gs::builtin_stage::input, gs::stage_side::before, noop()));

	const auto names = registry.stage_names();
	BOOST_REQUIRE(!names.empty());
	BOOST_TEST(names.front() == "begin");
	BOOST_TEST(names[1] == "after_begin");
}

BOOST_AUTO_TEST_CASE(BuiltinBodyCanBeSetOnlyOnce) {
	gs::frame_stage_registry registry;
	BOOST_TEST(registry.set_body(gs::builtin_stage::ui, noop()));
	BOOST_TEST(!registry.set_body(gs::builtin_stage::ui, noop()));
}

BOOST_AUTO_TEST_CASE(RunExecutesBodiesAndStagesInOrder) {
	gs::frame_stage_registry registry;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](gs::frame_step_context&) { trace += c; };
	};
	registry.set_body(gs::builtin_stage::begin, mark('B'));
	registry.set_body(gs::builtin_stage::update, mark('U'));
	registry.set_body(gs::builtin_stage::present, mark('P'));
	registry.add_stage("x", gs::builtin_stage::update, gs::stage_side::before, mark('x'));
	registry.add_stage("y", gs::builtin_stage::update, gs::stage_side::after, mark('y'));

	ds::app_data_storage storage;
	rnd::frame_context render;
	gs::frame_step_context ctx{ storage, std::chrono::duration<float>{ 0.016f }, render };
	registry.run(ctx);
	BOOST_TEST(trace == "BxUyP");
}

BOOST_AUTO_TEST_SUITE_END()
