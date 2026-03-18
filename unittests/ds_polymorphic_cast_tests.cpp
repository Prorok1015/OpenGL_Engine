#include <boost/test/unit_test.hpp>
#include "ds/ds_polymorphic_cast.hpp"
#include <memory>

struct Base {
    virtual ~Base() = default;
    int base_val = 1;
};

struct Derived : Base {
    int derived_val = 2;
};

struct OtherBase {
    virtual ~OtherBase() = default;
    int other_val = 3;
};

BOOST_AUTO_TEST_SUITE(DSPolymorphicCastTests)

BOOST_AUTO_TEST_CASE(RawPointerSuccess) {
    Derived derived_obj;
    Base* base_ptr = &derived_obj;
    Derived* derived_ptr = ds::polymorphic_cast<Derived>(base_ptr);
    BOOST_CHECK_EQUAL(derived_ptr, &derived_obj);
    BOOST_CHECK_EQUAL(derived_ptr->derived_val, 2);
}

BOOST_AUTO_TEST_CASE(RawReferenceSuccess) {
    Derived derived_obj;
    Base& base_ref = derived_obj;
    Derived& derived_ref = ds::polymorphic_cast<Derived>(base_ref);
    BOOST_CHECK_EQUAL(&derived_ref, &derived_obj);
    BOOST_CHECK_EQUAL(derived_ref.derived_val, 2);
}

BOOST_AUTO_TEST_CASE(SharedPointerSuccess) {
    std::shared_ptr<Base> base_ptr = std::make_shared<Derived>();
    std::shared_ptr<Derived> derived_ptr = ds::polymorphic_pointer_cast<Derived>(base_ptr);
    BOOST_CHECK(derived_ptr != nullptr);
    BOOST_CHECK_EQUAL(derived_ptr->derived_val, 2);
}

BOOST_AUTO_TEST_CASE(UniquePointerSuccess) {
    std::unique_ptr<Base> base_ptr = std::make_unique<Derived>();
    std::unique_ptr<Derived> derived_ptr = ds::polymorphic_cast<Derived>(std::move(base_ptr));
    BOOST_CHECK(derived_ptr != nullptr);
    BOOST_CHECK_EQUAL(derived_ptr->derived_val, 2);
    BOOST_CHECK(base_ptr == nullptr);
}

BOOST_AUTO_TEST_CASE(UniquePointerOwnershipTransferred) {
    auto original = std::make_unique<Derived>();
    Derived* raw = original.get();
    std::unique_ptr<Base> base_ptr = std::move(original);
    std::unique_ptr<Derived> derived_ptr = ds::polymorphic_cast<Derived>(std::move(base_ptr));
    BOOST_CHECK_EQUAL(derived_ptr.get(), raw);
}

BOOST_AUTO_TEST_SUITE_END()
