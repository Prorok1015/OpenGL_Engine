#include <boost/test/unit_test.hpp>
#include "resolvers/res_control_block.hpp"
#include <thread>
#include <string>

using namespace core::res;

BOOST_AUTO_TEST_SUITE(ResControlBlockTests)

// --- Status transitions ---

BOOST_AUTO_TEST_CASE(DefaultStatus_IsPending) {
	res_control_block<int> block;
	BOOST_CHECK(block.status == res_status::pending);
	BOOST_CHECK(!block.is_ready());
	BOOST_CHECK(!block.has_error());
}

BOOST_AUTO_TEST_CASE(SetReady_ChangesStatusToReady) {
	res_control_block<int> block;
	block.set_ready(42);
	BOOST_CHECK(block.is_ready());
	BOOST_CHECK(!block.has_error());
	BOOST_CHECK_EQUAL(block.data, 42);
}

BOOST_AUTO_TEST_CASE(SetError_ChangesStatusToError) {
	res_control_block<int> block;
	block.set_error("something went wrong");
	BOOST_CHECK(block.has_error());
	BOOST_CHECK(!block.is_ready());
	BOOST_CHECK_EQUAL(block.error_msg, "something went wrong");
}

BOOST_AUTO_TEST_CASE(Get_ReturnsDataWhenReady) {
	res_control_block<std::string> block;
	block.set_ready("hello");
	BOOST_CHECK_EQUAL(block.get(), "hello");
}

// --- Callbacks ---

BOOST_AUTO_TEST_CASE(Then_WhenAlreadyReady_CallsImmediately) {
	res_control_block<int> block;
	block.set_ready(10);

	bool called = false;
	block.then([&](auto& b) {
		called = true;
		BOOST_CHECK_EQUAL(b.data, 10);
	});
	BOOST_CHECK(called);
}

BOOST_AUTO_TEST_CASE(Then_WhenPending_DefersUntilReady) {
	res_control_block<int> block;

	bool called = false;
	block.then([&](auto& b) {
		called = true;
		BOOST_CHECK_EQUAL(b.data, 99);
	});
	BOOST_CHECK(!called);

	block.set_ready(99);
	BOOST_CHECK(called);
}

BOOST_AUTO_TEST_CASE(Then_MultipleCallbacks_AllFired) {
	res_control_block<int> block;

	int call_count = 0;
	block.then([&](auto&) { ++call_count; });
	block.then([&](auto&) { ++call_count; });
	block.then([&](auto&) { ++call_count; });

	block.set_ready(0);
	BOOST_CHECK_EQUAL(call_count, 3);
}

BOOST_AUTO_TEST_CASE(Then_OnError_CallbackStillFires) {
	res_control_block<int> block;

	bool called = false;
	block.then([&](auto& b) {
		called = true;
		BOOST_CHECK(b.has_error());
	});

	block.set_error("fail");
	BOOST_CHECK(called);
}

BOOST_AUTO_TEST_CASE(SetReady_ClearsCallbackList) {
	res_control_block<int> block;
	block.then([](auto&) {});
	block.set_ready(1);

	// Adding callback after ready should fire immediately, not accumulate
	int count = 0;
	block.then([&](auto&) { ++count; });
	BOOST_CHECK_EQUAL(count, 1);
}

// --- Thread safety ---

BOOST_AUTO_TEST_CASE(SetReady_FromAnotherThread) {
	auto block = std::make_shared<res_control_block<int>>();

	bool called = false;
	block->then([&](auto& b) {
		called = true;
		BOOST_CHECK_EQUAL(b.data, 77);
	});

	std::thread t([block]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		block->set_ready(77);
	});

	t.join();
	BOOST_CHECK(called);
	BOOST_CHECK(block->is_ready());
}

// --- Typed resource block ---

BOOST_AUTO_TEST_CASE(ResourceBlock_StoresSharedPtr) {
	res_resource_block block;
	BOOST_CHECK(block.status == res_status::pending);
	block.set_ready(nullptr);
	BOOST_CHECK(block.is_ready());
	BOOST_CHECK(block.data == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
