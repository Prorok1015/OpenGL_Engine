#include "core/core/common/ds/ds_fixed_vector.hpp"
#include <iostream>
#include <cassert>
#include <vector>

void test_fixed_vector_basic() {
    std::cout << "Running test_fixed_vector_basic..." << std::endl;
    ds::fixed_vector<int, 5> vec;
    
    assert(vec.empty());
    assert(vec.size() == 0);
    assert(vec.capacity() == 5);

    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);

    assert(!vec.empty());
    assert(vec.size() == 3);
    assert(vec[0] == 10);
    assert(vec[1] == 20);
    assert(vec[2] == 30);

    vec.pop_back();
    assert(vec.size() == 2);
    assert(vec.back() == 20);

    vec.clear();
    assert(vec.empty());
    std::cout << "test_fixed_vector_basic passed!" << std::endl;
}

void test_fixed_vector_overflow() {
    std::cout << "Running test_fixed_vector_overflow..." << std::endl;
    ds::fixed_vector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(2);
    
    try {
        vec.push_back(3);
        assert(false && "Should have thrown std::length_error");
    } catch (const std::length_error& e) {
        std::cout << "Caught expected exception: " << e.what() << std::endl;
    }
    std::cout << "test_fixed_vector_overflow passed!" << std::endl;
}

void test_fixed_vector_complex_types() {
    std::cout << "Running test_fixed_vector_complex_types..." << std::endl;
    struct tracker {
        static int instances;
        tracker() { instances++; }
        tracker(const tracker&) { instances++; }
        tracker(tracker&&) noexcept { instances++; }
        ~tracker() { instances--; }
    };
    int tracker::instances = 0;

    {
        ds::fixed_vector<tracker, 3> vec;
        vec.emplace_back();
        vec.emplace_back();
        assert(tracker::instances == 2);
        vec.pop_back();
        assert(tracker::instances == 1);
    }
    assert(tracker::instances == 0);
    std::cout << "test_fixed_vector_complex_types passed!" << std::endl;
}

int main() {
    try {
        test_fixed_vector_basic();
        test_fixed_vector_overflow();
        test_fixed_vector_complex_types();
        std::cout << "\nALL TESTS PASSED!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
