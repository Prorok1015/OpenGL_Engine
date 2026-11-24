#pragma once
#include <snk/common/engine_assert.h>
#include <cstddef>
#include <type_traits>
// Concept to ensure the type is an enum
template<typename T>
concept EnumType = std::is_enum_v<T>;

namespace ds {
    // A set of flags. The enum's values are the bit INDICES (my_flags::flag2 = 2 is bit 2), not masks, so an enum of up to
    // sizeof(underlying type) * 8 values makes a set that is exactly one underlying integer wide. Trivially copyable and
    // constexpr throughout: a component and a query filter may hold one by value.
    template<EnumType Enum>
    class bit_flags {

    public:
        using underlying_type = typename std::underlying_type<Enum>::type;

        constexpr bit_flags() = default;
        constexpr bit_flags(Enum flag) : flags_(shift_flag(flag)) {}
        constexpr bit_flags(const bit_flags& other) = default;
        constexpr bit_flags& operator=(const bit_flags& other) = default;

        // The set of exactly the bits of `raw`: what a desc schema and JSON read and write.
        static constexpr bit_flags from_raw(underlying_type raw) {
            bit_flags out;
            out.flags_ = raw;
            return out;
        }
        // Every bit of the underlying type set.
        static constexpr bit_flags all() { return from_raw(static_cast<underlying_type>(~static_cast<underlying_type>(0))); }

        constexpr underlying_type raw() const { return flags_; }

        constexpr bit_flags& operator=(Enum flag) {
            flags_ = shift_flag(flag);
            return *this;
        }

        constexpr bit_flags& operator|=(Enum flag) {
            flags_ |= shift_flag(flag);
            return *this;
        }

        // Keeps only `flag`: a set with that flag in it becomes that one flag, any other becomes empty.
        constexpr bit_flags& operator&=(Enum flag) {
            flags_ &= shift_flag(flag);
            return *this;
        }

        constexpr bit_flags& operator^=(Enum flag) {
            flags_ ^= shift_flag(flag);
            return *this;
        }

        constexpr bit_flags& operator|=(const bit_flags& mask) {
            flags_ |= mask.flags_;
            return *this;
        }

        constexpr bit_flags& operator&=(const bit_flags& mask) {
            flags_ &= mask.flags_;
            return *this;
        }

        constexpr bit_flags& operator^=(const bit_flags& mask) {
            flags_ ^= mask.flags_;
            return *this;
        }

        friend constexpr bit_flags operator|(bit_flags a, const bit_flags& b) { return a |= b; }
        friend constexpr bit_flags operator&(bit_flags a, const bit_flags& b) { return a &= b; }
        friend constexpr bit_flags operator^(bit_flags a, const bit_flags& b) { return a ^= b; }
        friend constexpr bit_flags operator~(const bit_flags& a) { return from_raw(static_cast<underlying_type>(~a.flags_)); }

        constexpr bool operator==(Enum flag) const {
            return flags_ == shift_flag(flag);
        }

        constexpr bool operator!=(Enum flag) const {
            return !(*this == flag);
        }

        friend constexpr bool operator==(const bit_flags& a, const bit_flags& b) { return a.flags_ == b.flags_; }
        friend constexpr bool operator!=(const bit_flags& a, const bit_flags& b) { return a.flags_ != b.flags_; }

        constexpr explicit operator bool() const {
            return flags_ != 0;
        }

        constexpr bool has_flag(Enum flag) const {
            return (flags_ & shift_flag(flag)) != 0;
        }

        // Whether the two sets share a flag (a layer mask test: `layers.any_of(mask)`).
        constexpr bool any_of(const bit_flags& mask) const {
            return (flags_ & mask.flags_) != 0;
        }

        // Whether every flag of `mask` is in this set.
        constexpr bool all_of(const bit_flags& mask) const {
            return (flags_ & mask.flags_) == mask.flags_;
        }

        constexpr void set(Enum flag, bool on = true) {
            if (on) flags_ |= shift_flag(flag);
            else flags_ &= static_cast<underlying_type>(~shift_flag(flag));
        }

        constexpr void reset(Enum flag) {
            set(flag, false);
        }

        constexpr void clear() {
            flags_ = 0;
        }

    private:
        underlying_type flags_ = 0;

        static constexpr underlying_type shift_flag(Enum flag) {
            ASSERT_MSG(static_cast<std::size_t>(flag) < sizeof(underlying_type) * 8, "bit_flags: flag {} does not fit in {} bits",
                static_cast<std::size_t>(flag), sizeof(underlying_type) * 8);
            return static_cast<underlying_type>(static_cast<underlying_type>(1) << static_cast<underlying_type>(flag));
        }
    };
}

// Example usage
//enum class my_flags {
//    none = 0,
//    flag1,
//    flag2,
//    flag3
//};
// 
//int main() {
//    bit_flags<my_flags> flags;
//    flags |= my_flags::flag1;
//    flags |= my_flags::flag2;
//    if (flags.has_flag(my_flags::flag1)) {
//    }
//   if (flags.has_flag(my_flags::flag3)) {
//    } else {
//    }
//    return 0;
//}
