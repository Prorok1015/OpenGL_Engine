#include <boost/test/unit_test.hpp>
#include "ds/ds_event.hpp"

// Simple test listener function
void simple_listener(int& value) {
    value = 10;
}

// Another listener for multiple subscriptions
void another_listener(int& value) {
    value += 5;
}

BOOST_AUTO_TEST_SUITE(DSEventTests)

// Test suite for Event with Simple Container Policy
BOOST_AUTO_TEST_SUITE(SimpleContainerEvent)

BOOST_AUTO_TEST_CASE(SubscriptionAndInvocation) {
    ds::Event<void(int&), ds::EventPolicySimpleContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    event += std::bind(&simple_listener, std::placeholders::_1);
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 10);
}

BOOST_AUTO_TEST_CASE(MultipleSubscriptions) {
    ds::Event<void(int&), ds::EventPolicySimpleContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    event += std::bind(&simple_listener, std::placeholders::_1);
    event += std::bind(&another_listener, std::placeholders::_1);
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 15);
}

BOOST_AUTO_TEST_CASE(UnsubscribeAll) {
    ds::Event<void(int&), ds::EventPolicySimpleContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    event += std::bind(&simple_listener, std::placeholders::_1);
    event.unsubscribe_all();
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 0);
}

BOOST_AUTO_TEST_CASE(EmptyEvent) {
    ds::Event<void(int&), ds::EventPolicySimpleContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 5;
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 5);
}

BOOST_AUTO_TEST_SUITE_END()

// Test suite for Event with Managed Container Policy
BOOST_AUTO_TEST_SUITE(ManagedContainerEvent)

BOOST_AUTO_TEST_CASE(SubscriptionWithHandle) {
    ds::Event<void(int&), ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    ds::Event<void(int&), ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>>::HANDLE handle = event.subscribe(std::bind(&simple_listener, std::placeholders::_1));
    BOOST_CHECK(event.is_handle_valid(handle));
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 10);
}

BOOST_AUTO_TEST_CASE(UnsubscribeByHandle) {
    ds::Event<void(int&), ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    auto handle = event.subscribe(std::bind(&simple_listener, std::placeholders::_1));
    event.subscribe(std::bind(&another_listener, std::placeholders::_1));
    event.unsubscribe(handle);
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 5);
}

struct MyTag {};

BOOST_AUTO_TEST_CASE(SubscriptionByTag) {
    ds::Event<void(int&), ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    event.subscribe_by_tag<MyTag>(std::bind(&simple_listener, std::placeholders::_1));
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 10);
}

BOOST_AUTO_TEST_CASE(UnsubscribeByTag) {
    ds::Event<void(int&), ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>> event;
    int test_value = 0;
    event.subscribe_by_tag<MyTag>(std::bind(&simple_listener, std::placeholders::_1));
    event.subscribe(std::bind(&another_listener, std::placeholders::_1));
    event.unsubscribe_by_tag<MyTag>();
    event.Invoke(test_value);
    BOOST_CHECK_EQUAL(test_value, 5);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
