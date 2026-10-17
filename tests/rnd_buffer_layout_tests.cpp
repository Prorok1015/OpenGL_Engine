#include <boost/test/unit_test.hpp>
#include "rnd_buffer_layout.h"

using namespace rnd::driver;

BOOST_AUTO_TEST_SUITE(RndBufferLayoutTests)

// --- shader_data_type_size ---

BOOST_AUTO_TEST_CASE(TypeSize_Float) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::FLOAT) == 4u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Vec2F) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::VEC2_F) == 8u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Vec3F) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::VEC3_F) == 12u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Vec4F) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::VEC4_F) == 16u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Mat3F) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::MAT3_F) == 36u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Mat4F) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::MAT4_F) == 64u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Int) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::INT) == 4u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Vec4I) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::VEC4_I) == 16u);
}

BOOST_AUTO_TEST_CASE(TypeSize_Bool) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::BOOL) == sizeof(bool));
}

BOOST_AUTO_TEST_CASE(TypeSize_Unknown_ReturnsZero) {
	BOOST_TEST(shader_data_type_size(SHADER_DATA_TYPE::UNKNOWN) == 0u);
}

// --- BufferElement::get_component_count ---

BOOST_AUTO_TEST_CASE(ComponentCount_Float_Is1) {
	BufferElement e(SHADER_DATA_TYPE::FLOAT, "pos");
	BOOST_TEST(e.get_component_count() == 1u);
}

BOOST_AUTO_TEST_CASE(ComponentCount_Vec3F_Is3) {
	BufferElement e(SHADER_DATA_TYPE::VEC3_F, "normal");
	BOOST_TEST(e.get_component_count() == 3u);
}

BOOST_AUTO_TEST_CASE(ComponentCount_Mat4F_Is4) {
	BufferElement e(SHADER_DATA_TYPE::MAT4_F, "transform");
	BOOST_TEST(e.get_component_count() == 4u);
}

BOOST_AUTO_TEST_CASE(ComponentCount_Bool_Is1) {
	BufferElement e(SHADER_DATA_TYPE::BOOL, "flag");
	BOOST_TEST(e.get_component_count() == 1u);
}

// --- BufferElement constructor ---

BOOST_AUTO_TEST_CASE(ElementConstructor_SetsFields) {
	BufferElement e(SHADER_DATA_TYPE::VEC3_F, "position", true);
	BOOST_TEST(e.Name == "position");
	BOOST_CHECK(e.Type == SHADER_DATA_TYPE::VEC3_F);
	BOOST_TEST(e.Size == 12u);
	BOOST_TEST(e.Offset == 0u);
	BOOST_TEST(e.Normalized == true);
}

// --- BufferLayout ---

BOOST_AUTO_TEST_CASE(Layout_Empty_StrideIsZero) {
	BufferLayout layout;
	BOOST_TEST(layout.get_stride() == 0u);
	BOOST_TEST(layout.get_elements().empty());
}

BOOST_AUTO_TEST_CASE(Layout_SingleElement_StrideEqualsSize) {
	BufferLayout layout = {
		{SHADER_DATA_TYPE::VEC3_F, "position"}
	};
	BOOST_TEST(layout.get_stride() == 12u);
}

BOOST_AUTO_TEST_CASE(Layout_MultipleElements_StrideIsSum) {
	BufferLayout layout = {
		{SHADER_DATA_TYPE::VEC3_F, "position"},
		{SHADER_DATA_TYPE::VEC2_F, "texcoord"},
		{SHADER_DATA_TYPE::VEC3_F, "normal"}
	};
	BOOST_TEST(layout.get_stride() == 12u + 8u + 12u);
}

BOOST_AUTO_TEST_CASE(Layout_OffsetsSequential) {
	BufferLayout layout = {
		{SHADER_DATA_TYPE::VEC3_F, "position"},
		{SHADER_DATA_TYPE::VEC2_F, "texcoord"},
		{SHADER_DATA_TYPE::FLOAT, "weight"}
	};
	auto& elems = layout.get_elements();
	BOOST_TEST(elems[0].Offset == 0u);
	BOOST_TEST(elems[1].Offset == 12u);
	BOOST_TEST(elems[2].Offset == 20u);
}

BOOST_AUTO_TEST_CASE(Layout_GetElementOffset_ByName) {
	BufferLayout layout = {
		{SHADER_DATA_TYPE::VEC3_F, "position"},
		{SHADER_DATA_TYPE::VEC2_F, "texcoord"}
	};
	BOOST_TEST(layout.get_element_offset("position") == 0u);
	BOOST_TEST(layout.get_element_offset("texcoord") == 12u);
}

BOOST_AUTO_TEST_CASE(Layout_GetElementOffset_NotFound_ReturnsZero) {
	BufferLayout layout = {
		{SHADER_DATA_TYPE::VEC3_F, "position"}
	};
	BOOST_TEST(layout.get_element_offset("nonexistent") == 0u);
}

BOOST_AUTO_TEST_CASE(Layout_Iterable) {
	BufferLayout layout = {
		{SHADER_DATA_TYPE::VEC3_F, "a"},
		{SHADER_DATA_TYPE::VEC2_F, "b"}
	};
	int count = 0;
	for (const auto& e : layout) {
		++count;
	}
	BOOST_TEST(count == 2);
}

BOOST_AUTO_TEST_SUITE_END()
