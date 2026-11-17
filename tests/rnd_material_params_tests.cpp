#include <boost/test/unit_test.hpp>
#include <snk/render/shader/rnd_material_params.h>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

// The MaterialParams block of set 2: where std140 puts its members, and how the explicit params list of a shader
// turns named values into the bytes of the block (no reflection of the GLSL source involved).

namespace {

using namespace rnd;

constexpr bool schema_fits(std::initializer_list<param_decl> decls, std::size_t block_size)
{
	return is_std140_schema(std::span<const param_decl>{ decls.begin(), decls.size() }, block_size);
}

}

BOOST_AUTO_TEST_SUITE(RndMaterialParamsTests)

BOOST_AUTO_TEST_CASE(TheBlockMatchesTheStd140LayoutOfMaterialParams)
{
	// MaterialParams { vec4 diffuseColor; vec4 emissiveColor; float shininess; } is 48 bytes (a block rounds up to 16).
	static_assert(offsetof(material_params, diffuse_color) == 0);
	static_assert(offsetof(material_params, emissive_color) == 16);
	static_assert(offsetof(material_params, shininess) == 32);
	static_assert(sizeof(material_params) == 48);
	BOOST_TEST(sizeof(material_params) % 16 == 0u);
	BOOST_TEST(is_std140_schema(phong_params, sizeof(material_params)));

	BOOST_REQUIRE(std::size(phong_params) == 3u);
	BOOST_TEST(phong_params[0].name == "diffuseColor");
	BOOST_TEST(phong_params[0].offset == offsetof(material_params, diffuse_color));
	BOOST_TEST((phong_params[0].type == param_type::vec4));
	BOOST_TEST(phong_params[1].name == "emissiveColor");
	BOOST_TEST(phong_params[1].offset == offsetof(material_params, emissive_color));
	BOOST_TEST(phong_params[2].name == "shininess");
	BOOST_TEST(phong_params[2].offset == offsetof(material_params, shininess));
	BOOST_TEST((phong_params[2].type == param_type::f32));
}

BOOST_AUTO_TEST_CASE(Std140AlignmentsAndSizes)
{
	BOOST_TEST(param_alignment(param_type::f32) == 4u);
	BOOST_TEST(param_alignment(param_type::vec2) == 8u);
	BOOST_TEST(param_alignment(param_type::vec3) == 16u);
	BOOST_TEST(param_alignment(param_type::vec4) == 16u);
	BOOST_TEST(param_size(param_type::vec3) == 12u);
	BOOST_TEST(param_size(param_type::vec4) == 16u);
}

BOOST_AUTO_TEST_CASE(ASchemaThatStd140WouldNotLayOutIsRefused)
{
	// Fine: a float may sit in the fourth component behind a vec3.
	BOOST_TEST(schema_fits({ { "a", param_type::vec3, 0 }, { "b", param_type::f32, 12 } }, 16));
	BOOST_TEST(schema_fits({ { "a", param_type::vec2, 8 }, { "b", param_type::vec2, 0 } }, 16));

	static_assert(!schema_fits({ { "a", param_type::vec4, 8 } }, 32));	// a vec4 starts at a multiple of 16
	static_assert(!schema_fits({ { "a", param_type::vec2, 4 } }, 16));	// a vec2 at a multiple of 8
	static_assert(!schema_fits({ { "a", param_type::vec4, 0 }, { "b", param_type::f32, 12 } }, 16));	// on top of each other
	static_assert(!schema_fits({ { "a", param_type::vec3, 0 }, { "b", param_type::vec3, 8 } }, 32));
	static_assert(!schema_fits({ { "a", param_type::vec4, 16 } }, 16));	// past the end
	static_assert(!schema_fits({ { "a", param_type::f32, 0 } }, 20));	// block not a multiple of 16
	static_assert(!schema_fits({ { "a", param_type::f32, 0 } }, 0));
	static_assert(!schema_fits({ { "a", param_type::f32, 0 }, { "a", param_type::f32, 4 } }, 16));	// one name twice
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(NamedValuesLandAtTheOffsetsOfTheSchema)
{
	const named_param values[] = {
		{ "shininess", 64.f },
		{ "diffuseColor", glm::vec4{ 0.1f, 0.2f, 0.3f, 0.4f } },
		{ "emissiveColor", glm::vec4{ 0.5f, 0.6f, 0.7f, 0.8f } },
	};
	const packed_params packed = pack_params(phong_params, sizeof(material_params), values);
	BOOST_TEST(packed.ok());
	BOOST_REQUIRE(packed.bytes.size() == sizeof(material_params));

	material_params expected;
	expected.diffuse_color = { 0.1f, 0.2f, 0.3f, 0.4f };
	expected.emissive_color = { 0.5f, 0.6f, 0.7f, 0.8f };
	expected.shininess = 64.f;
	BOOST_TEST(std::memcmp(packed.bytes.data(), &expected, sizeof(expected)) == 0);
	// The padding behind the float stays zero.
	for (std::size_t i = offsetof(material_params, shininess) + sizeof(float); i < packed.bytes.size(); ++i) {
		BOOST_REQUIRE(packed.bytes[i] == std::byte{ 0 });
	}
}

BOOST_AUTO_TEST_CASE(SmallerTypesFillTheirOwnBytesOnly)
{
	const param_decl schema[] = {
		{ "scale", param_type::vec2, 0 },
		{ "bias", param_type::vec2, 8 },
		{ "tint", param_type::vec3, 16 },
		{ "alpha", param_type::f32, 28 },
	};
	const named_param values[] = {
		{ "scale", glm::vec2{ 1.f, 2.f } },
		{ "bias", glm::vec2{ 3.f, 4.f } },
		{ "tint", glm::vec3{ 5.f, 6.f, 7.f } },
		{ "alpha", 8.f },
	};
	const packed_params packed = pack_params(schema, 32, values);
	BOOST_REQUIRE(packed.ok());
	float floats[8];
	BOOST_REQUIRE(packed.bytes.size() == sizeof(floats));
	std::memcpy(floats, packed.bytes.data(), sizeof(floats));
	for (int i = 0; i < 8; ++i) {
		BOOST_TEST(floats[i] == static_cast<float>(i + 1));
	}
}

BOOST_AUTO_TEST_CASE(AValueTheSchemaDoesNotMatchIsAnError)
{
	const named_param missing[] = { { "diffuseColor", glm::vec4{ 1.f } }, { "shininess", 1.f } };
	packed_params packed = pack_params(phong_params, sizeof(material_params), missing);
	BOOST_TEST(!packed.ok());
	BOOST_TEST(packed.error.find("no value for 'emissiveColor'") != std::string::npos);

	const named_param wrong_type[] = { { "diffuseColor", glm::vec3{ 1.f } }, { "emissiveColor", glm::vec4{ 1.f } }, { "shininess", 1.f } };
	packed = pack_params(phong_params, sizeof(material_params), wrong_type);
	BOOST_TEST(!packed.ok());
	BOOST_TEST(packed.error.find("'diffuseColor' is a vec4") != std::string::npos);

	const named_param extra[] = { { "diffuseColor", glm::vec4{ 1.f } }, { "emissiveColor", glm::vec4{ 1.f } }, { "shininess", 1.f }, { "roughness", 0.5f } };
	packed = pack_params(phong_params, sizeof(material_params), extra);
	BOOST_TEST(!packed.ok());
	BOOST_TEST(packed.error.find("'roughness' is not in the schema") != std::string::npos);

	const named_param all[] = { { "diffuseColor", glm::vec4{ 1.f } }, { "emissiveColor", glm::vec4{ 1.f } }, { "shininess", 1.f } };
	const param_decl overlapping[] = { { "a", param_type::vec4, 0 }, { "b", param_type::vec4, 8 } };
	packed = pack_params(overlapping, 32, all);
	BOOST_TEST(!packed.ok());
	BOOST_TEST(packed.bytes.empty());
}

BOOST_AUTO_TEST_SUITE_END()
