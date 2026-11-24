#include <boost/test/unit_test.hpp>
#include <snk/common/ds/ds_bit_frags.hpp>
#include <cstdint>
#include <type_traits>

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

// #1326: masks, raw access, constexpr and the layout a component can rely on.
namespace {
enum class layer : std::uint32_t {};

constexpr ds::bit_flags<layer> layer_bit(std::uint32_t index) { return ds::bit_flags<layer>(static_cast<layer>(index)); }

static_assert(std::is_trivially_copyable_v<ds::bit_flags<layer>>);
static_assert(std::is_standard_layout_v<ds::bit_flags<layer>>);
static_assert(sizeof(ds::bit_flags<layer>) == sizeof(std::uint32_t));
static_assert(alignof(ds::bit_flags<layer>) == alignof(std::uint32_t));
static_assert(sizeof(ds::bit_flags<test_flags>) == sizeof(std::underlying_type_t<test_flags>));
static_assert(layer_bit(3).raw() == 8u);
static_assert((layer_bit(0) | layer_bit(31)).raw() == 0x80000001u);
static_assert(ds::bit_flags<layer>::from_raw(6u).has_flag(static_cast<layer>(1)));
static_assert(ds::bit_flags<layer>::all().raw() == 0xFFFFFFFFu);
static_assert(!(layer_bit(2) & layer_bit(3)));
}

BOOST_AUTO_TEST_CASE(MaskOperators_CombineSetsBitByBit) {
    const auto a = layer_bit(1) | layer_bit(2);
    const auto b = layer_bit(2) | layer_bit(3);
    BOOST_CHECK_EQUAL((a & b).raw(), layer_bit(2).raw());
    BOOST_CHECK_EQUAL((a | b).raw(), (layer_bit(1) | layer_bit(2) | layer_bit(3)).raw());
    BOOST_CHECK_EQUAL((a ^ b).raw(), (layer_bit(1) | layer_bit(3)).raw());
    BOOST_CHECK_EQUAL((~a & ds::bit_flags<layer>::all()).raw(), 0xFFFFFFF9u);
    BOOST_CHECK(a == (layer_bit(2) | layer_bit(1)));
    BOOST_CHECK(a != b);
}

BOOST_AUTO_TEST_CASE(MaskAssignmentOperators_ChangeTheLeftSet) {
    auto set = layer_bit(0);
    set |= layer_bit(4) | layer_bit(5);
    BOOST_CHECK_EQUAL(set.raw(), 0x31u);
    set &= layer_bit(4) | layer_bit(5);
    BOOST_CHECK_EQUAL(set.raw(), 0x30u);
    set ^= layer_bit(5) | layer_bit(6);
    BOOST_CHECK_EQUAL(set.raw(), 0x50u);
}

BOOST_AUTO_TEST_CASE(AnyOfAndAllOf_TellOverlapFromContainment) {
    const auto layers = layer_bit(1) | layer_bit(2);
    BOOST_CHECK(layers.any_of(layer_bit(2) | layer_bit(9)));
    BOOST_CHECK(!layers.any_of(layer_bit(9)));
    BOOST_CHECK(!layers.any_of(ds::bit_flags<layer>{}));
    BOOST_CHECK(layers.all_of(layer_bit(1)));
    BOOST_CHECK(!layers.all_of(layer_bit(1) | layer_bit(9)));
    BOOST_CHECK(layers.all_of(ds::bit_flags<layer>{}));
}

BOOST_AUTO_TEST_CASE(SetAndReset_ChangeOneFlag) {
    ds::bit_flags<layer> flags;
    flags.set(static_cast<layer>(7));
    BOOST_CHECK(flags.has_flag(static_cast<layer>(7)));
    flags.set(static_cast<layer>(7), false);
    BOOST_CHECK(!flags);
    flags.set(static_cast<layer>(31));
    flags.set(static_cast<layer>(30));
    flags.reset(static_cast<layer>(31));
    BOOST_CHECK_EQUAL(flags.raw(), 0x40000000u);
}

BOOST_AUTO_TEST_CASE(RawAndFromRaw_RoundTrip_ForTheDescSchemaAndJson) {
    for (const std::uint32_t raw : { 0u, 1u, 0x80000000u, 0xDEADBEEFu, 0xFFFFFFFFu }) {
        BOOST_CHECK_EQUAL(ds::bit_flags<layer>::from_raw(raw).raw(), raw);
    }
    const auto flags = ds::bit_flags<layer>::from_raw(0b101u);
    BOOST_CHECK(flags.has_flag(static_cast<layer>(0)));
    BOOST_CHECK(!flags.has_flag(static_cast<layer>(1)));
    BOOST_CHECK(flags.has_flag(static_cast<layer>(2)));
}

BOOST_AUTO_TEST_CASE(AFlagAtTheTopBit_SurvivesAnEnumOfAnySize) {
    enum class byte_layer : std::uint8_t {};
    ds::bit_flags<byte_layer> flags(static_cast<byte_layer>(7));
    BOOST_CHECK_EQUAL(static_cast<unsigned>(flags.raw()), 0x80u);
    BOOST_CHECK_EQUAL(static_cast<unsigned>((~flags).raw()), 0x7Fu);
    BOOST_CHECK(flags.has_flag(static_cast<byte_layer>(7)));
}

BOOST_AUTO_TEST_SUITE_END()
