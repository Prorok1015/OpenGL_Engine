#include <boost/test/unit_test.hpp>
#include "ds/ds_bit_frags.hpp"

// Define a sample enum for testing
enum class test_flags {
    FLAG_A,
    FLAG_B,
    FLAG_C,
    FLAG_D
};

BOOST_AUTO_TEST_SUITE(DSBitFlagsTests)

BOOST_AUTO_TEST_CASE(DefaultConstructor) {
    ds::bit_flags<test_flags> flags;
    BOOST_CHECK(!static_cast<bool>(flags));
}

BOOST_AUTO_TEST_CASE(SingleFlagConstructor) {
    ds::bit_flags<test_flags> flags(test_flags::FLAG_A);
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_A));
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_B));
}

BOOST_AUTO_TEST_CASE(CopyConstructor) {
    ds::bit_flags<test_flags> original(test_flags::FLAG_B);
    original |= test_flags::FLAG_C;
    ds::bit_flags<test_flags> copy(original);
    BOOST_CHECK(copy.has_flag(test_flags::FLAG_B));
    BOOST_CHECK(copy.has_flag(test_flags::FLAG_C));
}

BOOST_AUTO_TEST_CASE(AssignmentOperator) {
    ds::bit_flags<test_flags> flags;
    flags = test_flags::FLAG_D;
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_D));
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_A));
}

BOOST_AUTO_TEST_CASE(BitwiseOrOperator) {
    ds::bit_flags<test_flags> flags(test_flags::FLAG_A);
    flags |= test_flags::FLAG_C;
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_A));
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_C));
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_B));
}

BOOST_AUTO_TEST_CASE(BitwiseAndOperator) {
    ds::bit_flags<test_flags> flags(test_flags::FLAG_A);
    flags |= test_flags::FLAG_B;
    flags &= test_flags::FLAG_A;
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_A));
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_B));
}

BOOST_AUTO_TEST_CASE(BitwiseXorOperator) {
    ds::bit_flags<test_flags> flags(test_flags::FLAG_A);
    flags |= test_flags::FLAG_B;
    flags ^= test_flags::FLAG_A;
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_A));
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_B));
}

BOOST_AUTO_TEST_CASE(EqualityOperator) {
    ds::bit_flags<test_flags> flags;
    flags = test_flags::FLAG_A;
    BOOST_CHECK(flags == test_flags::FLAG_A);
    BOOST_CHECK(flags != test_flags::FLAG_B);
}

BOOST_AUTO_TEST_CASE(BoolConversion) {
    ds::bit_flags<test_flags> flags_empty;
    ds::bit_flags<test_flags> flags_set(test_flags::FLAG_A);
    BOOST_CHECK(!static_cast<bool>(flags_empty));
    BOOST_CHECK(static_cast<bool>(flags_set));
}

BOOST_AUTO_TEST_CASE(HasFlag) {
    ds::bit_flags<test_flags> flags(test_flags::FLAG_B);
    BOOST_CHECK(flags.has_flag(test_flags::FLAG_B));
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_C));
}

BOOST_AUTO_TEST_CASE(Clear) {
    ds::bit_flags<test_flags> flags(test_flags::FLAG_A);
    flags |= test_flags::FLAG_D;
    flags.clear();
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_A));
    BOOST_CHECK(!flags.has_flag(test_flags::FLAG_D));
    BOOST_CHECK(!static_cast<bool>(flags));
}

BOOST_AUTO_TEST_SUITE_END()
