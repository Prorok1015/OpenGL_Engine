#include <boost/test/unit_test.hpp>
#include "ds/ds_fixed_vector.hpp"

BOOST_AUTO_TEST_SUITE(Core)

BOOST_AUTO_TEST_SUITE(Common)

BOOST_AUTO_TEST_SUITE(DS)

BOOST_AUTO_TEST_CASE(FixedVector)
{
    ds::fixed_vector<int, 10> vector;

    BOOST_TEST(vector.size() == 0);
    BOOST_TEST(vector.capacity() == 10);
    BOOST_TEST(vector.empty());

    vector.push_back(5);

    BOOST_TEST(vector.size() == 1);
    BOOST_TEST(!vector.empty());
    BOOST_TEST(vector[0] == 5);

    vector.push_back(10);

    BOOST_TEST(vector.size() == 2);
    BOOST_TEST(vector[0] == 5);
    BOOST_TEST(vector[1] == 10);

    vector.pop_back();

    BOOST_TEST(vector.size() == 1);
    BOOST_TEST(vector[0] == 5);

    vector.clear();

    BOOST_TEST(vector.size() == 0);
    BOOST_TEST(vector.empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
