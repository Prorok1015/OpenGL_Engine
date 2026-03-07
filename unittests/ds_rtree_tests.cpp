#include <boost/test/unit_test.hpp>
#include <vector>
#include <algorithm>
#include "engine_assert.h"
#include "ds/ds_rtree.h"
#include "ds/ds_bbox.hpp"

// Use the provided alias for a quadratic split r-tree
using RTree = ds::rtree_q<int, ds::bbox>;

BOOST_AUTO_TEST_SUITE(DSRTreeTests)

BOOST_AUTO_TEST_CASE(BuildAndQueryTest) {
    std::vector<std::pair<ds::bbox, int>> items;
    items.push_back({ds::bbox({0, 0}, {1, 1}), 1});
    items.push_back({ds::bbox({2, 2}, {3, 3}), 2});

    RTree rtree;
    rtree.build(items);

    // Query for the first item
    auto result1 = rtree.query({ds::bbox({0, 0}, {1, 1})});
    BOOST_CHECK_EQUAL(result1.size(), 1);
    BOOST_CHECK_EQUAL(result1[0], 1);

    // Query for the second item
    auto result2 = rtree.query({ds::bbox({2, 2}, {3, 3})});
    BOOST_CHECK_EQUAL(result2.size(), 1);
    BOOST_CHECK_EQUAL(result2[0], 2);

    // Query for an empty area
    auto result3 = rtree.query({ds::bbox({5, 5}, {6, 6})});
    BOOST_CHECK(result3.empty());
}

BOOST_AUTO_TEST_CASE(InsertAndQueryTest) {
    RTree rtree;
    rtree.insert({ds::bbox({0, 0}, {1, 1})}, 1);
    rtree.insert({ds::bbox({2, 2}, {3, 3})}, 2);

    // Query for the first item
    auto result1 = rtree.query({ds::bbox({0, 0}, {1, 1})});
    BOOST_CHECK_EQUAL(result1.size(), 1);
    BOOST_CHECK_EQUAL(result1[0], 1);

    // Query for the second item
    auto result2 = rtree.query({ds::bbox({2, 2}, {3, 3})});
    BOOST_CHECK_EQUAL(result2.size(), 1);
    BOOST_CHECK_EQUAL(result2[0], 2);

    // Query for an empty area
    auto result3 = rtree.query({ds::bbox({5, 5}, {6, 6})});
    BOOST_CHECK(result3.empty());
}

BOOST_AUTO_TEST_CASE(OverlappingQueryTest) {
    RTree rtree;
    rtree.build({
        {ds::bbox({0, 0}, {2, 2}), 1},
        {ds::bbox({1, 1}, {3, 3}), 2},
        {ds::bbox({4, 4}, {5, 5}), 3}
    });

    // Query overlapping the first two items
    auto results = rtree.query({ds::bbox({0.5, 0.5}, {1.5, 1.5})});
    BOOST_CHECK_EQUAL(results.size(), 2);
    std::sort(results.begin(), results.end());
    BOOST_CHECK_EQUAL(results[0], 1);
    BOOST_CHECK_EQUAL(results[1], 2);
}

BOOST_AUTO_TEST_SUITE_END()
