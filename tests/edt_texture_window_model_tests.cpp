#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_texture_window_model.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The texture as an editor asset (#1205): texture_2d_desc is a data document, and the texture window's model reads the
// header as the runtime does and writes the sampling and the source through the document, one record each.

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

struct texture_fixture : test_util::scene_env
{
	texture_fixture()
		: test_util::scene_env(test_util::test_descs::without)
	{
		fs::create_directories(res_root);
		fs::create_directories(lower);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		engine::render::register_render_descs(*descs);
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		// The engine's base texture, as the project's own here: a lower layer's file is no data document.
		fs::copy_file(fs::path(SNK_CORE_RES_DIR) / "base_texture.desc", res_root / "base_texture.desc", fs::copy_options::overwrite_existing);
		put("pictures/rock.png", "not decoded here");
	}

	~texture_fixture()
	{
		document.reset();
		engine::render::unregister_render_descs(*descs);
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	res::tag put(std::string_view name, std::string_view content) const
	{
		fs::create_directories((res_root / std::string(name)).parent_path());
		std::ofstream out(res_root / std::string(name), std::ios::binary);
		out << content;
		return res::tag{ "res://" + std::string(name) };
	}

	std::string read(std::string_view name) const
	{
		std::ifstream in(res_root / std::string(name), std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	edt::data_document& open(const res::tag& tag)
	{
		document.reset();
		document.emplace(services, layout());
		const edt::open_result opened = document->open(tag);
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		return *document;
	}

	static const json::object& own_header(const edt::data_document& data)
	{
		static const json::object none;
		const json::value* header = data.own().if_contains("header");
		return header && header->is_object() ? header->as_object() : none;
	}

	test_util::temp_dir dir{ "edt_texture_window_model" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	edt::editor_services services;
	std::optional<edt::data_document> document;
};

// What the image import writes beside a picture (edt_image_import.cpp), with some sampling of its own.
constexpr const char* k_imported = R"json({"__type":"texture_2d_desc","__version":1,"__parent":"res://base_texture.desc",
	"name":"rock","data":"res://pictures/rock.png",
	"header":{"wrap":"repeat","min":"linear_mipmap_nearest","data":{"mip_levels":4}}})json";

// One red texel held in the file (#1204).
constexpr const char* k_inline = R"json({"__type":"texture_2d_desc","__version":1,"__parent":"res://base_texture.desc",
	"name":"red","pixels":{"width":1,"height":1,"format":"RGBA8","base64":"/wAA/w=="}})json";

}

BOOST_FIXTURE_TEST_SUITE(EdtTextureWindowModelTests, texture_fixture)

BOOST_AUTO_TEST_CASE(TheTexture2dDesc_IsADocumentType_WithItsHeaderAndPixelsAsCustomFields)
{
	const std::vector<std::string> types = descs->document_types();
	BOOST_TEST(std::ranges::count(types, std::string{ "texture_2d_desc" }) == 1);
	const desc::type_schema* schema = descs->find_schema("texture_2d_desc");
	BOOST_REQUIRE(schema != nullptr);
	BOOST_TEST(schema->is_document);
	const desc::field_schema* header = schema->find_field("/header");
	BOOST_REQUIRE(header != nullptr);
	BOOST_TEST(header->editor_hidden);
	const desc::field_schema* pixels = schema->find_field("/pixels");
	BOOST_REQUIRE(pixels != nullptr);
	BOOST_TEST(pixels->read_only);
	const desc::field_schema* data = schema->find_field("/data");
	BOOST_REQUIRE(data != nullptr);
	BOOST_TEST((data->kind == desc::field_kind::res_tag));
	// The runtime still makes a texture_2d_desc of such a file.
	BOOST_TEST(descs->create_instance("texture_2d_desc", res::tag{ "res://x.txm.desc" }) != nullptr);
}

BOOST_AUTO_TEST_CASE(TheBaseTexture_AnImportedOne_AndInlinePixels_OpenAsDocuments_AndAreSavedUnchanged)
{
	const res::tag imported = put("pictures/rock.txm.desc", k_imported);
	const res::tag held = put("red.txm.desc", k_inline);
	for (const res::tag& tag : { res::tag{ "res://base_texture.desc" }, imported, held }) {
		BOOST_TEST_CONTEXT(tag.view()) {
			const std::string before = read(tag.relative());
			edt::data_document& data = open(tag);
			BOOST_TEST(data.type() == "texture_2d_desc");
			BOOST_TEST(!data.is_dirty());
			const edt::data_document::save_form form = data.save_bytes();
			BOOST_TEST(form.error.empty(), form.error);
			BOOST_TEST(data.save().saved);
			BOOST_TEST(read(tag.relative()) == before);
		}
	}
}

BOOST_AUTO_TEST_CASE(TheEffectiveHeader_IsTheOwnOverTheParentsOverTheDefaults)
{
	edt::data_document& data = open(put("pictures/rock.txm.desc", k_imported));
	const edt::texture_window_model model(data);
	BOOST_TEST(model.wrap() == "repeat");
	BOOST_TEST(model.min_filter() == "linear_mipmap_nearest");
	BOOST_TEST(model.mag_filter() == "nearest");
	BOOST_TEST(model.mip_levels() == 4);
	const json::object header = model.effective_header();
	BOOST_TEST(header.at("data").as_object().at("format").as_string() == "RGBA8");
	BOOST_TEST(header.at("usage").as_string() == "sampled");

	// Without a parent or a header of its own a texture reads the runtime's defaults.
	edt::data_document& bare = open(put("bare.txm.desc", R"json({"__type":"texture_2d_desc","__version":1})json"));
	const edt::texture_window_model defaults(bare);
	BOOST_TEST(defaults.wrap() == "clamp_to_edge");
	BOOST_TEST(defaults.min_filter() == "linear");
	BOOST_TEST(defaults.mag_filter() == "linear");
	BOOST_TEST(defaults.mip_levels() == 1);
}

BOOST_AUTO_TEST_CASE(EachSamplingChange_WritesTheWholeHeader_AsOneRecord_AndIsSaved)
{
	const res::tag tag = put("pictures/rock.txm.desc", k_imported);
	edt::data_document& data = open(tag);
	edt::texture_window_model model(data);
	const std::size_t records = data.history().size();

	BOOST_REQUIRE(model.set_wrap("repeat_mirror"));
	BOOST_TEST(data.history().size() == records + 1);
	BOOST_TEST((own_header(data) == model.effective_header()));
	BOOST_TEST(own_header(data).at("mag").as_string() == "nearest");
	BOOST_TEST(own_header(data).at("data").as_object().at("format").as_string() == "RGBA8");

	BOOST_REQUIRE(model.set_min("nearest"));
	BOOST_REQUIRE(model.set_mag("linear"));
	BOOST_REQUIRE(model.set_mip_levels(0));
	BOOST_TEST(data.history().size() == records + 4);
	BOOST_TEST(model.mip_levels() == 1);
	// The same value again, or one that is no option, changes nothing.
	BOOST_TEST(!model.set_mip_levels(1));
	BOOST_TEST(!model.set_wrap("repeat_mirror"));
	BOOST_TEST(!model.set_wrap("spiral"));
	BOOST_TEST(data.history().size() == records + 4);

	BOOST_REQUIRE(data.save().saved);
	edt::data_document& again = open(tag);
	const edt::texture_window_model reread(again);
	BOOST_TEST(reread.wrap() == "repeat_mirror");
	BOOST_TEST(reread.min_filter() == "nearest");
	BOOST_TEST(reread.mag_filter() == "linear");
	BOOST_TEST(reread.mip_levels() == 1);
	BOOST_TEST(std::string(again.own().at("data").as_string()) == "res://pictures/rock.png");
}

BOOST_AUTO_TEST_CASE(AnUndo_TakesASamplingChangeBack)
{
	edt::data_document& data = open(put("pictures/rock.txm.desc", k_imported));
	edt::texture_window_model model(data);
	BOOST_REQUIRE(model.set_wrap("clamp_to_border"));
	BOOST_REQUIRE(data.undo());
	BOOST_TEST(model.wrap() == "repeat");
	BOOST_TEST(!data.is_dirty());
}

BOOST_AUTO_TEST_CASE(UseFile_NamesThePicture_AndDropsTheInlinePixels_InOneRecord)
{
	const res::tag tag = put("red.txm.desc", k_inline);
	edt::data_document& data = open(tag);
	edt::texture_window_model model(data);
	edt::texture_source source = model.source();
	BOOST_TEST((source.from == edt::texture_source::kind::inline_pixels));
	BOOST_TEST(edt::texture_source_text(source) == "inline 1x1 RGBA8");
	const std::size_t records = data.history().size();

	BOOST_REQUIRE(model.use_file(res::tag{ "res://pictures/rock.png" }));
	BOOST_TEST(data.history().size() == records + 1);
	BOOST_TEST(std::string(data.own().at("data").as_string()) == "res://pictures/rock.png");
	const json::value* pixels = data.own().if_contains("pixels");
	BOOST_TEST((pixels == nullptr || pixels->is_null()));
	source = model.source();
	BOOST_TEST((source.from == edt::texture_source::kind::file));
	BOOST_TEST(source.file == "res://pictures/rock.png");

	BOOST_REQUIRE(data.save().saved);
	const json::object written = json::parse(read("red.txm.desc")).as_object();
	BOOST_TEST(std::string(written.at("data").as_string()) == "res://pictures/rock.png");
	BOOST_TEST((!written.contains("pixels") || written.at("pixels").is_null()));
}

BOOST_AUTO_TEST_SUITE_END()
