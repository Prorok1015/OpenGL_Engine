#include <boost/test/unit_test.hpp>
#include "ds/ds_fixed_vector.hpp"
#include <string>

BOOST_AUTO_TEST_SUITE(DSFixedVectorTests)

BOOST_AUTO_TEST_CASE(DefaultConstructor_EmptyAndCorrectCapacity) {
	ds::fixed_vector<int, 5> v;
	BOOST_TEST(v.size() == 0);
	BOOST_TEST(v.capacity() == 5);
	BOOST_TEST(v.empty());
}

BOOST_AUTO_TEST_CASE(InitializerListConstructor) {
	ds::fixed_vector<int, 5> v = {1, 2, 3};
	BOOST_TEST(v.size() == 3);
	BOOST_TEST(v[0] == 1);
	BOOST_TEST(v[1] == 2);
	BOOST_TEST(v[2] == 3);
}

BOOST_AUTO_TEST_CASE(InitializerListOverflow_Throws) {
	BOOST_CHECK_THROW(
		(ds::fixed_vector<int, 2>{1, 2, 3}),
		std::length_error
	);
}

BOOST_AUTO_TEST_CASE(PushBack_IncreasesSize) {
	ds::fixed_vector<int, 3> v;
	v.push_back(10);
	v.push_back(20);
	BOOST_TEST(v.size() == 2);
	BOOST_TEST(v[0] == 10);
	BOOST_TEST(v[1] == 20);
}

BOOST_AUTO_TEST_CASE(PushBack_WhenFull_Throws) {
	ds::fixed_vector<int, 2> v = {1, 2};
	BOOST_CHECK_THROW(v.push_back(3), std::length_error);
}

BOOST_AUTO_TEST_CASE(EmplaceBack_ConstructsInPlace) {
	ds::fixed_vector<std::string, 3> v;
	v.emplace_back("hello");
	v.emplace_back(5, 'x');
	BOOST_TEST(v[0] == "hello");
	BOOST_TEST(v[1] == "xxxxx");
}

BOOST_AUTO_TEST_CASE(PopBack_DecreasesSize) {
	ds::fixed_vector<int, 5> v = {1, 2, 3};
	v.pop_back();
	BOOST_TEST(v.size() == 2);
	BOOST_TEST(v.back() == 2);
}

BOOST_AUTO_TEST_CASE(Clear_MakesEmpty) {
	ds::fixed_vector<int, 5> v = {1, 2, 3};
	v.clear();
	BOOST_TEST(v.empty());
	BOOST_TEST(v.size() == 0);
}

BOOST_AUTO_TEST_CASE(FrontAndBack) {
	ds::fixed_vector<int, 5> v = {10, 20, 30};
	BOOST_TEST(v.front() == 10);
	BOOST_TEST(v.back() == 30);
}

BOOST_AUTO_TEST_CASE(AtBoundsCheck_Throws) {
	ds::fixed_vector<int, 3> v = {1};
	BOOST_CHECK_EQUAL(v.at(0), 1);
	BOOST_CHECK_THROW(v.at(1), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(Iterator_RangeFor) {
	ds::fixed_vector<int, 5> v = {1, 2, 3};
	int sum = 0;
	for (int x : v) sum += x;
	BOOST_TEST(sum == 6);
}

BOOST_AUTO_TEST_CASE(CopyConstructor) {
	ds::fixed_vector<int, 5> v1 = {1, 2, 3};
	ds::fixed_vector<int, 5> v2(v1);
	BOOST_TEST(v1 == v2);
	v2.push_back(4);
	BOOST_TEST(v1.size() == 3);
}

BOOST_AUTO_TEST_CASE(MoveConstructor) {
	ds::fixed_vector<int, 5> v1 = {1, 2, 3};
	ds::fixed_vector<int, 5> v2(std::move(v1));
	BOOST_TEST(v2.size() == 3);
	BOOST_TEST(v1.empty());
}

BOOST_AUTO_TEST_CASE(Insert_AtMiddle) {
	ds::fixed_vector<int, 5> v = {1, 3};
	v.insert(v.begin() + 1, 2);
	BOOST_TEST(v.size() == 3);
	BOOST_TEST(v[0] == 1);
	BOOST_TEST(v[1] == 2);
	BOOST_TEST(v[2] == 3);
}

BOOST_AUTO_TEST_CASE(Erase_AtMiddle) {
	ds::fixed_vector<int, 5> v = {1, 2, 3};
	v.erase(v.begin() + 1);
	BOOST_TEST(v.size() == 2);
	BOOST_TEST(v[0] == 1);
	BOOST_TEST(v[1] == 3);
}

BOOST_AUTO_TEST_CASE(EqualityOperator) {
	ds::fixed_vector<int, 5> a = {1, 2, 3};
	ds::fixed_vector<int, 5> b = {1, 2, 3};
	ds::fixed_vector<int, 5> c = {1, 2, 4};
	BOOST_TEST(a == b);
	BOOST_TEST(!(a == c));
}

BOOST_AUTO_TEST_SUITE_END()
