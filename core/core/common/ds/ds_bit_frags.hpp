#pragma once
#include <type_traits>
#include <iostream>
// Concept to ensure the type is an enum
template<typename T>
concept EnumType = std::is_enum_v<T>;

namespace ds {
    template<EnumType Enum>
    class bit_flags {

    public:
        using underlying_type = typename std::underlying_type<Enum>::type;

        bit_flags() : flags_(0) {}
        bit_flags(Enum flag) : flags_(shift_flag(flag)) {}
        bit_flags(const bit_flags& other) : flags_(other.flags_) {}

        bit_flags& operator=(Enum flag) {
            flags_ = shift_flag(flag);
            return *this;
        }

        bit_flags& operator|=(Enum flag) {
            flags_ |= shift_flag(flag);
            return *this;
        }

        bit_flags& operator&=(Enum flag) {
            flags_ &= shift_flag(flag);
            return *this;
        }

        bit_flags& operator^=(Enum flag) {
            flags_ ^= shift_flag(flag);
            return *this;
        }

        bool operator==(Enum flag) const {
            return flags_ == shift_flag(flag);
        }

        bool operator!=(Enum flag) const {
            return !(*this == flag);
        }

        explicit operator bool() const {
            return flags_ != 0;
        }

        bool has_flag(Enum flag) const {
            return (flags_ & shift_flag(flag)) != 0;
        }

        void clear() {
            flags_ = 0;
        }

    private:
        underlying_type flags_;

        static underlying_type shift_flag(Enum flag) {
            return static_cast<underlying_type>(1) << static_cast<underlying_type>(flag);
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
//        std::cout << "flag1 is set" << std::endl;
//    }
//   if (flags.has_flag(my_flags::flag3)) {
//        std::cout << "flag3 is set" << std::endl;
//    } else {
//        std::cout << "flag3 is not set" << std::endl;
//    }
//    return 0;
//}
