#include <boost/test/unit_test.hpp>
#if SNK_HAS_ASSIMP
#include "null_engine_fixture.hpp"
#include <snk/command/cmd_base64.h>
#include <snk/model_import/mdl_import.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_image_format.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #1206: embedded textures are named by their place in the model, never by a name the model gives them; every one is
// checked against the limits before it is copied; texels go as RGBA, rows in the order their reader takes them.
namespace {

namespace json = boost::json;

constexpr std::string_view k_red_1x1 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
constexpr std::string_view k_green_2x1 = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAADUlEQVR4nGNg+M8ARAAHAAH/YKtA0QAAAABJRU5ErkJggg==";
constexpr std::string_view k_blue_1x2 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAACCAIAAAAW4yFwAAAADklEQVR4nGNgYPjPAMQABgIB/xARhfEAAAAASUVORK5CYII=";
// A PNG header that promises 100000 x 100000 pixels, and nothing to decode.
constexpr std::string_view k_huge_png = "iVBORw0KGgoAAAANSUhEUgABhqAAAYagCAYAAACoUgvIAAAAAElFTkSuQmCC";
constexpr std::string_view k_tga_1x1 = "AAACAAAAAAAAAAAAAQABACAIAAD//w==";
constexpr std::string_view k_gif = "R0lGODlhAQABAAAAADs=";

static_assert(mdl::k_max_inline_texture_bytes == res::image_format::max_inline_pixel_bytes);
static_assert(mdl::k_max_inline_texture_side == res::image_format::max_inline_pixel_side);

std::vector<std::byte> decoded(std::string_view base64)
{
	std::optional<std::vector<std::byte>> bytes = cmd::base64::decode(base64);
	BOOST_REQUIRE(bytes.has_value());
	return *bytes;
}

class memory_files final : public mdl::file_source
{
public:
	memory_files(std::string name, std::string_view text) : m_name(std::move(name))
	{
		m_bytes.resize(text.size());
		std::memcpy(m_bytes.data(), text.data(), text.size());
	}

	mdl::opened_file open(std::string_view name) override
	{
		if (name != m_name) return {};
		return { &m_bytes, {} };
	}

	bool exists(std::string_view name) override { return name == m_name; }

private:
	std::string m_name;
	std::vector<std::byte> m_bytes;
};

struct memory_sink final : mdl::import_sink
{
	bool exists(const res::tag& tag) override { return stored.contains(std::string(tag.view())); }
	void store(const res::tag& tag, const std::vector<std::byte>& bytes) override { stored[std::string(tag.view())] = bytes; }

	std::map<std::string, std::vector<std::byte>> stored;

	std::set<std::string> embedded() const
	{
		std::set<std::string> out;
		for (const auto& [tag, bytes] : stored) {
			if (tag.find("__embedded_txm/") != std::string::npos) out.insert(tag);
		}
		return out;
	}

	json::object desc(const std::string& tag) const
	{
		const auto found = stored.find(tag);
		BOOST_REQUIRE(found != stored.end());
		return json::parse(std::string_view(reinterpret_cast<const char*>(found->second.data()), found->second.size())).as_object();
	}
};

// One triangle drawn once per image, each with a material whose base colour is that image (a data URI, as glTF embeds).
std::string gltf_with_images(const std::vector<std::pair<std::string, std::string>>& images)
{
	std::string primitives, materials, textures, list;
	for (std::size_t i = 0; i < images.size(); ++i) {
		const std::string comma = i == 0 ? "" : ",";
		primitives += std::format(R"({}{{"attributes":{{"POSITION":0}},"material":{}}})", comma, i);
		materials += std::format(R"({}{{"pbrMetallicRoughness":{{"baseColorTexture":{{"index":{}}}}}}})", comma, i);
		textures += std::format(R"({}{{"source":{}}})", comma, i);
		list += std::format(R"({}{{"name":"{}","uri":"{}"}})", comma, images[i].first, images[i].second);
	}
	return std::format(R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0}}],)"
		R"("meshes":[{{"primitives":[{}]}}],"materials":[{}],"textures":[{}],"images":[{}],)"
		R"("buffers":[{{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}}],)"
		R"("bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":36}}],)"
		R"("accessors":[{{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}}]}})",
		primitives, materials, textures, list);
}

std::string png_uri(std::string_view base64) { return std::format("data:image/png;base64,{}", base64); }

mdl::import_result import_text(const std::string& name, std::string_view text, memory_sink& sink)
{
	memory_files files(name, text);
	mdl::import_options options;
	options.display_name = name;
	options.geometry_tag = res::tag{ "memory://emb/m.geom.desc" };
	options.prefab_tag = res::tag{ "memory://emb/m.prefab.desc" };
	options.embedded_folder = "emb/__embedded_txm/";
	return mdl::import_model(name, files, sink, options);
}

bool any_contains(const std::vector<std::string>& lines, std::string_view part)
{
	return std::ranges::any_of(lines, [part](const std::string& line) { return line.find(part) != std::string::npos; });
}

// Two meshes, each with a material whose diffuse texture is an embedded picture called "tex.png", one in a/, one in b/.
std::string fbx_with_two_textures_of_one_name()
{
	std::string objects;
	std::string connections;
	const std::string_view pictures[] = { k_red_1x1, k_green_2x1 };
	for (int i = 0; i < 2; ++i) {
		const int id = 1000 * (i + 1);
		const char folder = static_cast<char>('a' + i);
		objects += std::format(R"(	Geometry: {0}1, "Geometry::g{1}", "Mesh" {{
		Vertices: *9 {{
			a: 0,0,0,1,0,0,0,1,0
		}}
		PolygonVertexIndex: *3 {{
			a: 0,1,-3
		}}
		GeometryVersion: 124
		LayerElementMaterial: 0 {{
			Version: 101
			Name: ""
			MappingInformationType: "AllSame"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 {{
				a: 0
			}}
		}}
		Layer: 0 {{
			Version: 100
			LayerElement:  {{
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}}
		}}
	}}
	Model: {0}2, "Model::m{1}", "Mesh" {{
		Version: 232
	}}
	Material: {0}3, "Material::mat{1}", "" {{
		Version: 102
		ShadingModel: "phong"
	}}
	Texture: {0}4, "Texture::t{1}", "" {{
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::t{1}"
		FileName: "{2}/tex.png"
		RelativeFilename: "{2}/tex.png"
	}}
	Video: {0}5, "Video::v{1}", "Clip" {{
		Type: "Clip"
		FileName: "{2}/tex.png"
		RelativeFilename: "{2}/tex.png"
		Content: "{3}"
	}}
)", id / 1000, i, folder, pictures[i]);
		connections += std::format(R"(	C: "OO",{0}1,{0}2
	C: "OO",{0}2,0
	C: "OO",{0}3,{0}2
	C: "OP",{0}4,{0}3, "DiffuseColor"
	C: "OO",{0}5,{0}4
)", id / 1000);
	}
	return std::format(R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {{
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}}
GlobalSettings:  {{
	Version: 1000
}}
Objects:  {{
{}}}
Connections:  {{
{}}}
)", objects, connections);
}

struct texel_upload_fixture : test_util::null_engine_fixture
{
	texel_upload_fixture()
		: null_engine_fixture(false)
		, render(storage.require<rnd::render_system>())
		, gpu(dynamic_cast<rnd::driver::null::driver&>(*render.get_driver()))
		, textures(render.get_texture_manager())
	{
		gpu.observe_texture_uploads([this](rnd::driver::texture_handle, const void* data, const rnd::driver::texture_region& region) {
			const auto* bytes = static_cast<const unsigned char*>(data);
			uploads.emplace_back(bytes, bytes + static_cast<std::size_t>(region.width) * region.height * 4);
		});
	}
	~texel_upload_fixture() { gpu.observe_texture_uploads({}); }

	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	// The pixels the texture `desc` uploads, rows in the order the GPU gets them (bottom first).
	std::vector<unsigned char> uploaded(const res::tag& desc)
	{
		uploads.clear();
		rnd::texture_id id = textures.require(desc);
		for (int frame = 0; frame < 10 && !textures.is_ready(id); ++frame) {
			test_util::pump(tasks);
			id = textures.require(desc);
		}
		BOOST_REQUIRE(textures.is_ready(id));
		BOOST_REQUIRE(uploads.size() == 1u);
		return uploads.front();
	}

	void store_json(const res::tag& tag, const json::object& body) { BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(json::serialize(body)))); }

	rnd::render_system& render;
	rnd::driver::null::driver& gpu;
	rnd::texture_manager& textures;
	std::vector<std::vector<unsigned char>> uploads;
};

// Texels of `width` x `height`, b, g, r, a: the top row is red, every other row blue.
std::vector<std::byte> red_top_blue_rest(std::uint32_t width, std::uint32_t height)
{
	std::vector<std::byte> texels(std::size_t{ width } * height * 4);
	for (std::size_t i = 0; i < texels.size(); i += 4) {
		const bool top = i < std::size_t{ width } * 4;
		texels[i + 0] = std::byte{ static_cast<unsigned char>(top ? 0 : 255) };	// b
		texels[i + 1] = std::byte{ 0 };	// g
		texels[i + 2] = std::byte{ static_cast<unsigned char>(top ? 255 : 0) };	// r
		texels[i + 3] = std::byte{ 255 };	// a
	}
	return texels;
}

}

BOOST_AUTO_TEST_SUITE(MdlEmbeddedTexture)

BOOST_AUTO_TEST_CASE(ImagesWithoutANameWithOneNameOrWithAPath_GetANameEachFromTheirIndex)
{
	memory_sink sink;
	const std::string model = gltf_with_images(
		{ { "", png_uri(k_red_1x1) }, { "", png_uri(k_green_2x1) }, { "../../evil", png_uri(k_blue_1x2) } });
	const mdl::import_result result = import_text("m.gltf", model, sink);

	BOOST_REQUIRE(result.error.empty());
	BOOST_TEST(result.missing_textures.empty());
	const std::set<std::string> expected = { "memory://emb/__embedded_txm/m.texture_0.pct", "memory://emb/__embedded_txm/m.texture_0.txm.desc",
		"memory://emb/__embedded_txm/m.texture_1.pct", "memory://emb/__embedded_txm/m.texture_1.txm.desc",
		"memory://emb/__embedded_txm/m.texture_2.pct", "memory://emb/__embedded_txm/m.texture_2.txm.desc" };
	BOOST_TEST((sink.embedded() == expected));
	for (const auto& [tag, bytes] : sink.stored) BOOST_TEST(tag.find("evil") == std::string::npos, tag);

	// Each picture once, as the model holds it, and each desc names its own.
	std::set<std::vector<std::byte>> pictures;
	for (int i = 0; i < 3; ++i) {
		const std::string data = std::format("memory://emb/__embedded_txm/m.texture_{}.pct", i);
		pictures.insert(sink.stored.at(data));
		const json::object desc = sink.desc(std::format("memory://emb/__embedded_txm/m.texture_{}.txm.desc", i));
		BOOST_TEST(desc.at("data").as_string() == data);
		BOOST_TEST(!desc.contains("pixels"));
	}
	BOOST_TEST((pictures == std::set<std::vector<std::byte>>{ decoded(k_red_1x1), decoded(k_green_2x1), decoded(k_blue_1x2) }));

	// The same model again: the same tags and bytes.
	memory_sink again;
	BOOST_REQUIRE(import_text("m.gltf", model, again).error.empty());
	BOOST_TEST((again.stored == sink.stored));
}

BOOST_AUTO_TEST_CASE(APictureOverTheLimitsOrOfAnotherKind_IsSkippedWithItsReason_TheRestIsImported)
{
	memory_sink sink;
	const mdl::import_result result = import_text("m.gltf",
		gltf_with_images({ { "huge", png_uri(k_huge_png) }, { "gif", std::format("data:image/gif;base64,{}", k_gif) },
			{ "ok", png_uri(k_red_1x1) } }), sink);

	BOOST_REQUIRE(result.error.empty());
	BOOST_TEST(result.missing_textures.size() == 2u);
	BOOST_TEST(any_contains(result.missing_textures, "*0"));
	BOOST_TEST(any_contains(result.missing_textures, "*1"));
	const std::set<std::string> expected = { "memory://emb/__embedded_txm/m.texture_2.pct", "memory://emb/__embedded_txm/m.texture_2.txm.desc" };
	BOOST_TEST((sink.embedded() == expected));
}

BOOST_AUTO_TEST_CASE(ATgaWithAHeaderItsDecoderReads_IsAPctLikeTheOthers)
{
	memory_sink sink;
	const mdl::import_result result = import_text("m.gltf",
		gltf_with_images({ { "t", std::format("data:image/x-tga;base64,{}", k_tga_1x1) } }), sink);

	BOOST_REQUIRE(result.error.empty());
	BOOST_TEST(result.missing_textures.empty());
	BOOST_TEST((sink.stored.at("memory://emb/__embedded_txm/m.texture_0.pct") == decoded(k_tga_1x1)));
}

BOOST_AUTO_TEST_CASE(TwoFbxPicturesOfOneFileName_AreTwoTextures)
{
	memory_sink sink;
	const mdl::import_result result = import_text("m.fbx", fbx_with_two_textures_of_one_name(), sink);

	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	const std::set<std::string> expected = { "memory://emb/__embedded_txm/m.texture_0.pct", "memory://emb/__embedded_txm/m.texture_0.txm.desc",
		"memory://emb/__embedded_txm/m.texture_1.pct", "memory://emb/__embedded_txm/m.texture_1.txm.desc" };
	BOOST_TEST((sink.embedded() == expected));
	BOOST_TEST((std::set<std::vector<std::byte>>{ sink.stored.at("memory://emb/__embedded_txm/m.texture_0.pct"),
		sink.stored.at("memory://emb/__embedded_txm/m.texture_1.pct") } == std::set<std::vector<std::byte>>{ decoded(k_red_1x1), decoded(k_green_2x1) }));
}

BOOST_AUTO_TEST_CASE(SmallTexels_AreInlinePixelsOfRgba_TopRowFirst)
{
	// b, g, r, a: red, green on top; blue, white below.
	const std::vector<std::byte> texels = { std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 0 }, std::byte{ 255 },
		std::byte{ 0 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 },
		std::byte{ 255 }, std::byte{ 255 } };
	const mdl::stored_texels stored = mdl::store_texels(texels, 2, 2);

	BOOST_TEST(stored.raw.empty());
	BOOST_TEST(stored.pixels.at("width").as_int64() == 2);
	BOOST_TEST(stored.pixels.at("height").as_int64() == 2);
	BOOST_TEST(stored.pixels.at("format").as_string() == "RGBA8");
	const std::vector<std::byte> rgba = decoded(std::string_view(stored.pixels.at("base64").as_string()));
	const std::vector<std::byte> expected = { std::byte{ 255 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 }, std::byte{ 0 }, std::byte{ 255 },
		std::byte{ 0 }, std::byte{ 255 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 },
		std::byte{ 255 }, std::byte{ 255 } };
	BOOST_TEST((rgba == expected));
}

BOOST_AUTO_TEST_CASE(TexelsPastTheInlineLimit_AreARawFile_BottomRowFirst)
{
	// 128 x 129 x 4 = 66048 bytes: past 64 KiB.
	const mdl::stored_texels stored = mdl::store_texels(red_top_blue_rest(128, 129), 128, 129);

	BOOST_TEST(stored.pixels.empty());
	BOOST_REQUIRE(stored.raw.size() == res::image_format::raw_header_size + 128u * 129u * 4u);
	res::image_format::raw_header header;
	std::memcpy(&header, stored.raw.data(), res::image_format::raw_header_size);
	BOOST_TEST(header.size.x == 128);
	BOOST_TEST(header.size.y == 129);
	BOOST_TEST(header.channels == 4);
	const std::byte* last_row = stored.raw.data() + res::image_format::raw_header_size + 128u * 128u * 4u;
	BOOST_TEST((last_row[0] == std::byte{ 255 } && last_row[2] == std::byte{ 0 }));	// red, the model's top row
	const std::byte* first_row = stored.raw.data() + res::image_format::raw_header_size;
	BOOST_TEST((first_row[0] == std::byte{ 0 } && first_row[2] == std::byte{ 255 }));	// blue
}

BOOST_AUTO_TEST_SUITE_END()

// The two forms reach the GPU alike: the model's top row is the last row uploaded, as for a picture file.
BOOST_FIXTURE_TEST_SUITE(MdlEmbeddedTextureUpload, texel_upload_fixture)

BOOST_AUTO_TEST_CASE(InlineAndRawTexels_UploadTheModelsTopRowLast)
{
	const mdl::stored_texels small = mdl::store_texels(red_top_blue_rest(2, 2), 2, 2);
	BOOST_REQUIRE(small.raw.empty());
	const res::tag inline_desc{ "memory://emb_upload/small.txm.desc" };
	store_json(inline_desc, { { "__type", "texture_2d_desc" }, { "pixels", small.pixels } });
	const std::vector<unsigned char> inline_pixels = uploaded(inline_desc);
	BOOST_REQUIRE(inline_pixels.size() == 16u);
	BOOST_TEST((std::vector<unsigned char>(inline_pixels.begin(), inline_pixels.begin() + 4) == std::vector<unsigned char>{ 0, 0, 255, 255 }));
	BOOST_TEST((std::vector<unsigned char>(inline_pixels.end() - 4, inline_pixels.end()) == std::vector<unsigned char>{ 255, 0, 0, 255 }));

	const mdl::stored_texels big = mdl::store_texels(red_top_blue_rest(128, 129), 128, 129);
	BOOST_REQUIRE(!big.raw.empty());
	const res::tag raw{ "memory://emb_upload/big.raw" };
	BOOST_REQUIRE(resources().store(raw, big.raw));
	const res::tag raw_desc{ "memory://emb_upload/big.txm.desc" };
	store_json(raw_desc, { { "__type", "texture_2d_desc" }, { "data", std::string(raw.view()) } });
	const std::vector<unsigned char> raw_pixels = uploaded(raw_desc);
	BOOST_REQUIRE(raw_pixels.size() == 128u * 129u * 4u);
	BOOST_TEST((std::vector<unsigned char>(raw_pixels.begin(), raw_pixels.begin() + 4) == std::vector<unsigned char>{ 0, 0, 255, 255 }));
	BOOST_TEST((std::vector<unsigned char>(raw_pixels.end() - 4, raw_pixels.end()) == std::vector<unsigned char>{ 255, 0, 0, 255 }));
}

BOOST_AUTO_TEST_SUITE_END()
#endif
