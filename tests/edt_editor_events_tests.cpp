#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_editor_events.h>
#include <string>
#include <type_traits>
#include <vector>

static_assert(!std::is_copy_constructible_v<edt::connection>);
static_assert(std::is_nothrow_move_constructible_v<edt::connection>);

BOOST_AUTO_TEST_SUITE(EditorEventsTests)

BOOST_AUTO_TEST_CASE(DeliversArguments)
{
	edt::editor_events events;
	edt::document_id got_doc = 0;
	std::uint64_t got_generation = 0;
	auto c = events.document_changed.subscribe([&](edt::document_id d, std::uint64_t g) {
		got_doc = d;
		got_generation = g;
	});
	events.document_changed.emit(7, 42);
	BOOST_CHECK_EQUAL(got_doc, 7u);
	BOOST_CHECK_EQUAL(got_generation, 42u);
}

BOOST_AUTO_TEST_CASE(EmitWithoutSubscribersIsNoop)
{
	edt::editor_events events;
	events.selection_changed.emit();
	events.level_reloaded.emit();
	BOOST_CHECK_EQUAL(events.selection_changed.subscriber_count(), 0u);
}

BOOST_AUTO_TEST_CASE(SubscribersRunInSubscriptionOrder)
{
	edt::signal<> sig;
	std::string order;
	auto a = sig.subscribe([&] { order += 'a'; });
	auto b = sig.subscribe([&] { order += 'b'; });
	auto c = sig.subscribe([&] { order += 'c'; });
	sig.emit();
	sig.emit();
	BOOST_CHECK_EQUAL(order, "abcabc");
}

BOOST_AUTO_TEST_CASE(ConnectionDestructorUnsubscribes)
{
	edt::signal<int> sig;
	int calls = 0;
	{
		auto c = sig.subscribe([&](int) { ++calls; });
		BOOST_CHECK(c.connected());
		sig.emit(1);
		BOOST_CHECK_EQUAL(sig.subscriber_count(), 1u);
	}
	sig.emit(1);
	BOOST_CHECK_EQUAL(calls, 1);
	BOOST_CHECK_EQUAL(sig.subscriber_count(), 0u);
}

BOOST_AUTO_TEST_CASE(ExplicitDisconnectAndMove)
{
	edt::signal<> sig;
	int calls = 0;
	auto a = sig.subscribe([&] { ++calls; });
	edt::connection moved = std::move(a);
	BOOST_CHECK(!a.connected());
	BOOST_CHECK(moved.connected());
	sig.emit();
	moved.disconnect();
	moved.disconnect();
	sig.emit();
	BOOST_CHECK_EQUAL(calls, 1);
}

BOOST_AUTO_TEST_CASE(MoveAssignmentDisconnectsPrevious)
{
	edt::signal<> sig;
	int first = 0, second = 0;
	auto c = sig.subscribe([&] { ++first; });
	c = sig.subscribe([&] { ++second; });
	sig.emit();
	BOOST_CHECK_EQUAL(first, 0);
	BOOST_CHECK_EQUAL(second, 1);
	BOOST_CHECK_EQUAL(sig.subscriber_count(), 1u);
}

BOOST_AUTO_TEST_CASE(UnsubscribeDuringEmitIsSafe)
{
	edt::signal<> sig;
	std::vector<int> calls;
	edt::connection second;
	auto first = sig.subscribe([&] {
		calls.push_back(1);
		second.disconnect(); // a later slot removed mid-emit must not run
	});
	second = sig.subscribe([&] { calls.push_back(2); });
	auto third = sig.subscribe([&] { calls.push_back(3); });
	sig.emit();
	BOOST_CHECK((calls == std::vector<int>{ 1, 3 }));
}

BOOST_AUTO_TEST_CASE(SlotMayUnsubscribeItself)
{
	edt::signal<> sig;
	edt::connection self;
	int self_calls = 0;
	self = sig.subscribe([&] {
		++self_calls;
		self.disconnect();
	});
	sig.emit();
	sig.emit();
	BOOST_CHECK_EQUAL(self_calls, 1);
	BOOST_CHECK_EQUAL(sig.subscriber_count(), 0u);
}

BOOST_AUTO_TEST_CASE(SubscribeDuringEmitRunsOnNextEmit)
{
	edt::signal<> sig;
	int late_calls = 0;
	edt::connection late;
	auto first = sig.subscribe([&] {
		if (!late.connected()) {
			late = sig.subscribe([&] { ++late_calls; });
		}
	});
	sig.emit();
	BOOST_CHECK_EQUAL(late_calls, 0);
	sig.emit();
	BOOST_CHECK_EQUAL(late_calls, 1);
}

BOOST_AUTO_TEST_CASE(ConnectionOutlivingSignalIsHarmless)
{
	edt::connection c;
	{
		edt::signal<> sig;
		c = sig.subscribe([] {});
	}
	BOOST_CHECK(c.connected());
	c.disconnect();
	BOOST_CHECK(!c.connected());
}

BOOST_AUTO_TEST_SUITE_END()
