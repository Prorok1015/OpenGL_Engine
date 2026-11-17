#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_model_importer.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// What an agent's import reads besides the model, found by parsing a crafted glTF with assimp (#548).

namespace {

namespace fs = std::filesystem;

const std::string png_bytes = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, 'x');

std::string gltf_with_texture(const std::string& uri)
{
	return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
		R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)"
		R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)" + std::string(48, 'A') + R"("}],)"
		R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
		R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]}],)"
		R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
		R"("textures":[{"source":0}],"images":[{"uri":")" + uri + R"("}]})";
}

struct scan_fixture
{
	edt::model_file_scan scan(const std::string& uri)
	{
		dir.write("models/robot.gltf", gltf_with_texture(uri));
		const fs::path model = dir.path() / "models" / "robot.gltf";
		std::ifstream in(model, std::ios::binary);
		const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		bytes.assign(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size());
		edt::import_source source;
		source.path = model;
		source.preloaded = &bytes;
		return edt::model_importer::scan_files(source);
	}

	test_util::temp_dir dir{ "agent_model_scan" };
	std::vector<std::byte> bytes;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentModelScanTests, scan_fixture)

BOOST_AUTO_TEST_CASE(ATextureBesideTheModel_IsListedForThePerson)
{
	dir.write("models/skin.png", png_bytes);
	const edt::model_file_scan found = scan("skin.png");
	BOOST_REQUIRE_MESSAGE(found.ok(), found.error);
	BOOST_REQUIRE_EQUAL(found.files.size(), 1u);
	BOOST_CHECK_EQUAL(found.files[0].size, png_bytes.size());
	BOOST_CHECK_EQUAL(found.total_bytes, png_bytes.size());
}

BOOST_AUTO_TEST_CASE(AModelThatNamesAHiddenFile_IsRefused)
{
	dir.write("models/.ssh/id_rsa", "-----BEGIN PRIVATE KEY-----");
	const edt::model_file_scan found = scan(".ssh/id_rsa");
	BOOST_CHECK(!found.ok());
	BOOST_CHECK(found.files.empty());
}

BOOST_AUTO_TEST_CASE(AModelThatNamesAFileOutsideItsFolder_IsRefused)
{
	dir.write("secret.png", png_bytes);
	const edt::model_file_scan found = scan("../secret.png");
	BOOST_CHECK(!found.ok());
}

BOOST_AUTO_TEST_CASE(ATextureThatIsNoImage_IsRefused)
{
	dir.write("models/config.json", "{\"token\":1}");
	const edt::model_file_scan found = scan("config.json");
	BOOST_CHECK(!found.ok());
	dir.write("models/liar.png", "not really a png");
	BOOST_CHECK(!scan("liar.png").ok());
}

BOOST_AUTO_TEST_SUITE_END()
