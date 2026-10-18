#include <boost/test/unit_test.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct test_service {
    int value = 42;
};

struct another_test_service {
    float f_value = 3.14f;
};

struct named_service {
    static constexpr std::string_view service_id = "my.service";
};

struct order_a {
    explicit order_a(std::vector<std::string>* log_) : log(log_) {}
    ~order_a() { log->push_back("a"); }
    std::vector<std::string>* log;
};

struct order_b {
    explicit order_b(std::vector<std::string>* log_) : log(log_) {}
    ~order_b() { log->push_back("b"); }
    std::vector<std::string>* log;
};

struct base_iface {
    virtual ~base_iface() = default;
    virtual int id() const = 0;
};

struct derived_impl : base_iface {
    int id() const override { return 7; }
};

BOOST_AUTO_TEST_SUITE(DSStoreTests)

BOOST_AUTO_TEST_CASE(TypeKeyDistinctTypesDiffer)
{
    BOOST_CHECK(ds::type_key<test_service>() != ds::type_key<another_test_service>());
}

BOOST_AUTO_TEST_CASE(TypeKeySameTypeIsStable)
{
    BOOST_CHECK_EQUAL(ds::type_key<test_service>(), ds::type_key<test_service>());
    static_assert(ds::type_key<int>() == ds::type_key<int>());
}

BOOST_AUTO_TEST_CASE(TypeKeyUsesServiceId)
{
    BOOST_CHECK_EQUAL(ds::type_key<named_service>(), ds::fnv1a_64("my.service"));
}

BOOST_AUTO_TEST_CASE(SharedPolicyBasicOperations)
{
    ds::app_data_storage storage;

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
    BOOST_CHECK_EQUAL(&child_service, &child_storage.require<another_test_service>());
    BOOST_CHECK(child_storage.has_value<test_service>());
    test_service& required_from_child = child_storage.require<test_service>();
    BOOST_CHECK_EQUAL(&parent_service, &required_from_child);

    // Parent should NOT be able to access child's services
    BOOST_CHECK(!parent_storage.has_value<another_test_service>());
}

BOOST_AUTO_TEST_CASE(RequireMissingServiceThrows)
{
    ds::app_data_storage storage;
    BOOST_CHECK_THROW(storage.require<test_service>(), std::runtime_error);
    BOOST_CHECK(storage.require_shared<test_service>() == nullptr);

    try {
        storage.require<test_service>();
        BOOST_FAIL("require must throw");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK(std::string{ e.what() }.find("test_service") != std::string::npos);
    }

    // Lookup through a parent that also lacks the service must throw too.
    ds::app_data_storage child(&storage);
    BOOST_CHECK_THROW(child.require<test_service>(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DestroysInReverseRegistrationOrder)
{
    std::vector<std::string> log;
    {
        ds::app_data_storage storage;
        storage.construct<order_a>(&log);
        storage.construct<order_b>(&log);
    }
    BOOST_CHECK(log == (std::vector<std::string>{ "b", "a" }));
}

BOOST_AUTO_TEST_CASE(DuplicateConstructThrows)
{
    ds::app_data_storage storage;
    test_service& first = storage.construct<test_service>();
    BOOST_CHECK_THROW(storage.construct<test_service>(), std::logic_error);
    BOOST_CHECK_EQUAL(&storage.require<test_service>(), &first);
    BOOST_CHECK_EQUAL(storage.size(), 1u);
}

BOOST_AUTO_TEST_CASE(DestructMissingThrows)
{
    ds::app_data_storage storage;
    BOOST_CHECK_THROW(storage.destruct<test_service>(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(DestructKeepsOtherEntriesReachable)
{
    ds::app_data_storage storage;
    storage.construct<test_service>();
    another_test_service& other = storage.construct<another_test_service>();
    storage.destruct<test_service>();
    BOOST_CHECK(!storage.has_value<test_service>());
    BOOST_CHECK_EQUAL(&storage.require<another_test_service>(), &other);
    BOOST_CHECK_EQUAL(storage.size(), 1u);
}

BOOST_AUTO_TEST_CASE(ConstructBaseKeyFromDerivedInstance)
{
    ds::app_data_storage storage;
    base_iface& iface = storage.construct<base_iface, derived_impl>();
    BOOST_CHECK_EQUAL(iface.id(), 7);
    BOOST_CHECK(storage.has_value<base_iface>());
    BOOST_CHECK(!storage.has_value<derived_impl>());
    BOOST_CHECK_EQUAL(&storage.require<base_iface>(), &iface);
}

BOOST_AUTO_TEST_SUITE_END()
