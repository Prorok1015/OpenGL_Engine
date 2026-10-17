#include <boost/test/unit_test.hpp>
#include "adapters/res_text_adapter.h"
#include <cstring>

BOOST_AUTO_TEST_SUITE(ResTextAdapterTests)

static std::vector<std::byte> to_bytes(const std::string& str) {
	std::vector<std::byte> result(str.size());
	std::memcpy(result.data(), str.data(), str.size());
	return result;
}

// --- text_resource ---

BOOST_AUTO_TEST_CASE(TextResource_ConstructFromBytes) {
	auto tag = res::tag::make("test/hello.txt");
	auto bytes = to_bytes("Hello, World!");
	res::text_resource resource(tag, bytes);
	BOOST_CHECK_EQUAL(resource.str(), "Hello, World!");
	BOOST_CHECK(resource.get_tag() == tag);
}

BOOST_AUTO_TEST_CASE(TextResource_EmptyContent) {
	auto tag = res::tag::make("test/empty.txt");
	std::vector<std::byte> empty;
	res::text_resource resource(tag, empty);
	BOOST_CHECK(resource.str().empty());
}

BOOST_AUTO_TEST_CASE(TextResource_CStr) {
	auto tag = res::tag::make("test/data.txt");
	auto bytes = to_bytes("test data");
	res::text_resource resource(tag, bytes);
	BOOST_CHECK(std::strcmp(resource.c_str(), "test data") == 0);
}

// --- text_adapter deserialize ---

BOOST_AUTO_TEST_CASE(Adapter_Deserialize_ReturnsTextResource) {
	res::text_adapter adapter;
	auto tag = res::tag::make("shaders/basic.vert");
	auto bytes = to_bytes("#version 330 core\nvoid main() {}");

	auto result = adapter.deserialize(tag, bytes);
	BOOST_CHECK(result != nullptr);

	auto text_res = std::dynamic_pointer_cast<res::text_resource>(result);
	BOOST_CHECK(text_res != nullptr);
	BOOST_CHECK_EQUAL(text_res->str(), "#version 330 core\nvoid main() {}");
}

BOOST_AUTO_TEST_CASE(Adapter_Deserialize_PreservesTag) {
	res::text_adapter adapter;
	auto tag = res::tag::make("config/settings.json");
	auto bytes = to_bytes("{}");

	auto result = adapter.deserialize(tag, bytes);
	BOOST_CHECK(result->get_tag() == tag);
}

// --- text_adapter serialize ---

BOOST_AUTO_TEST_CASE(Adapter_Serialize_Roundtrip) {
	res::text_adapter adapter;
	auto tag = res::tag::make("test/roundtrip.txt");
	std::string original = "The quick brown fox jumps over the lazy dog";
	auto bytes = to_bytes(original);

	// Deserialize
	auto resource = adapter.deserialize(tag, bytes);

	// Serialize back
	auto serialized = adapter.serialize(tag, resource);

	// Compare
	BOOST_CHECK_EQUAL(serialized.size(), bytes.size());
	BOOST_CHECK(std::memcmp(serialized.data(), bytes.data(), bytes.size()) == 0);
}

BOOST_AUTO_TEST_CASE(Adapter_Serialize_EmptyContent) {
	res::text_adapter adapter;
	auto tag = res::tag::make("test/empty.txt");
	std::vector<std::byte> empty;

	auto resource = adapter.deserialize(tag, empty);
	auto serialized = adapter.serialize(tag, resource);
	BOOST_CHECK(serialized.empty());
}

// --- adapter_info ---

BOOST_AUTO_TEST_CASE(AdapterInfo_TextAdapter) {
	auto info = res::text_adapter::INFO;
	BOOST_CHECK(info.resource_type == ds::type_id::make<res::text_resource>());
}

BOOST_AUTO_TEST_SUITE_END()
