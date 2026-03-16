#include <boost/test/unit_test.hpp>
#include "ds/ds_store.hpp"

struct test_service {
    int value = 42;
};

struct another_test_service {
    float f_value = 3.14f;
};

BOOST_AUTO_TEST_SUITE(DSStoreTests)

BOOST_AUTO_TEST_CASE(SharedPolicyBasicOperations)
{
    ds::data_storage_t<ds::shared_storage_policy> storage;

    // Test construction
    test_service& service = storage.construct<test_service>();
    BOOST_CHECK_EQUAL(service.value, 42);

    // Test has_value
    BOOST_CHECK(storage.has_value<test_service>());
    BOOST_CHECK(!storage.has_value<another_test_service>());

    // Test require
    test_service& required_service = storage.require<test_service>();
    BOOST_CHECK_EQUAL(&service, &required_service);

    // Test require_shared
    std::shared_ptr<test_service> shared_service = storage.require_shared<test_service>();
    BOOST_CHECK(shared_service != nullptr);
    BOOST_CHECK_EQUAL(shared_service.get(), &service);

    std::shared_ptr<another_test_service> non_existing_service = storage.require_shared<another_test_service>();
    BOOST_CHECK(non_existing_service == nullptr);

    // Test destruction
    storage.destruct<test_service>();
    BOOST_CHECK(!storage.has_value<test_service>());
}

BOOST_AUTO_TEST_CASE(HierarchicalLookup)
{
    ds::app_data_storage parent_storage;
    test_service& parent_service = parent_storage.construct<test_service>();

    ds::app_data_storage child_storage(&parent_storage);
    another_test_service& child_service = child_storage.construct<another_test_service>();

    // Child should be able to access parent's services
    BOOST_CHECK(child_storage.has_value<test_service>());
    test_service& required_from_child = child_storage.require<test_service>();
    BOOST_CHECK_EQUAL(&parent_service, &required_from_child);

    // Parent should NOT be able to access child's services
    BOOST_CHECK(!parent_storage.has_value<another_test_service>());
}

BOOST_AUTO_TEST_SUITE_END()
