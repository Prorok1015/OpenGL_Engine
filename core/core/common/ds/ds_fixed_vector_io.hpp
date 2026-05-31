#pragma once
// Streaming for ds::fixed_vector. Kept out of ds_fixed_vector.hpp so the hot core
// header does not pull in <ostream>. Include this only where you actually stream.
#include "ds_fixed_vector.hpp"
#include <ostream>
#include <cstddef>

namespace ds
{
    template <typename T, std::size_t N>
    std::ostream& operator<<(std::ostream& os, const fixed_vector<T, N>& v) {
        os << "[";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i > 0) os << ", ";
            os << v[i];
        }
        os << "]";
        return os;
    }
}
