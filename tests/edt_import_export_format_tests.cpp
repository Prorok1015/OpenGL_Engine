#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_asset_exporter.h"
#include "edt_async_import_task.h"
#include "edt_model_importer.h"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/resource/res_desc_binary.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// What an import exports into the project (#559): text descs in the editor's own JSON style, the geometry on one line, and
// an unchanged model imported again writes nothing.
namespace {

namespace json = boost::json;

constexpr const char* OBJ_TRIANGLE = R"(mtllib tri.mtl
v -1 -1 0
v 1 -1 0
v 0 1 0
o Tri
usemtl paint
f 1 2 3
)";

constexpr const char* MTL_PAINT = R"(newmtl paint
Kd 0.25 0.5 1
Ns 32
)";

std::string text_of(const std::vector<std::byte>& bytes)
{
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct export_format_fixture : test_util::scene_env
{
	export_format_fixture()
	{
		static std::atomic<int> counter{ 0 };
		// Unique per run too: a folder left by an earlier run would hold the files already.
		const auto run = std::format("{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
		dir = std::filesystem::temp_directory_path() / ("snk_import_export_format_" + run);
		std::filesystem::create_directories(dir);
		model = dir / "tri.obj";
		std::ofstream(model) << OBJ_TRIANGLE;
		std::ofstream(dir / "tri.mtl") << MTL_PAINT;
		folder = "tests/import_format_" + run;
	}

	~export_format_fixture()
	{
		std::error_code ignored;
		std::filesystem::remove_all(dir, ignored);
		// scene_env's res:// root is ./res.
		std::filesystem::remove_all(std::filesystem::path{ "res" } / folder, ignored);
	}

	// Imports the model and exports it into res://<folder>/; `written` gets the files the export wrote.
	void import_and_export(std::vector<res::tag>& written)
	{
		edt::model_importer importer(*descs, *rs);
		edt::import_source source;
		source.path = model;
		const edt::import_intermediate imported = importer.import_background(source);
		BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
		edt::import_result result;
		result.root_tag = imported.prefab_tag;
		result.all_tags = imported.all_tags;
		result.source_path = imported.source_path;

		edt::asset_exporter exporter(*rs);
		const auto remap = exporter.build_remap(result, "tri", folder);
		BOOST_REQUIRE(exporter.export_to_project(result, remap, nullptr, nullptr, &written, true));
		for (const auto& [from, to] : remap) {
			if (std::ranges::find(exported, to) == exported.end()) exported.push_back(to);
		}
	}

	std::map<std::string, std::string> files_on_disk() const
	{
		std::map<std::string, std::string> out;
		for (const res::tag& tag : exported) {
			auto data = rs->fetch_data(tag);
			BOOST_REQUIRE(data);
			const std::vector<std::byte>& bytes = data->get();
			BOOST_REQUIRE_MESSAGE(!data->has_error(), tag.string());
			out[tag.string()] = text_of(bytes);
		}
		return out;
	}

	// The geometry of `tag` read the way the exporter reads it (arrays, or the binary file beside it).
	rnd::geometry_desc geometry_of(const res::tag& tag)
	{
		auto data = rs->fetch_data(tag);
		BOOST_REQUIRE(data);
		const std::vector<std::byte>& bytes = data->get();
		BOOST_REQUIRE(!data->has_error());
		rnd::geometry_desc out;
		const std::string error = rnd::read_geometry_file(*rs, tag, json::parse(text_of(bytes)).as_object(), out);
		BOOST_REQUIRE_MESSAGE(error.empty(), error);
		return out;
	}

	std::filesystem::path dir;
	std::filesystem::path model;
	std::string folder;
	std::vector<res::tag> exported;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtImportExportFormatTests, export_format_fixture)

BOOST_AUTO_TEST_CASE(TheExportedTextDescs_AreInTheEditorsStyle_TheGeometryIsOneLine)
{
	std::vector<res::tag> written;
	import_and_export(written);

	bool prefab = false;
	bool material = false;
	bool geometry = false;
	for (const auto& [tag, text] : files_on_disk()) {
		if (!tag.ends_with(".desc")) continue;
		BOOST_TEST_CONTEXT(tag) {
			const json::value parsed = json::parse(text);
			BOOST_TEST(text.find("memory://") == std::string::npos);
			if (tag.ends_with(".geom.desc")) {
				geometry = true;
				BOOST_TEST(std::ranges::count(text, '\n') <= 1);
			} else {
				prefab = prefab || tag.ends_with(".prefab.desc");
				material = material || tag.ends_with(".mat.desc");
				BOOST_TEST(std::ranges::count(text, '\n') > 2);
				BOOST_TEST(text == text_of(desc::pretty_json_bytes(parsed)));
			}
		}
	}
	BOOST_TEST(prefab);
	BOOST_TEST(material);
	BOOST_TEST(geometry);
}

BOOST_AUTO_TEST_CASE(TheSameModelImportedAgain_WritesNothing_AndAnEditedFileIsWrittenAgain)
{
	std::vector<res::tag> first;
	import_and_export(first);
	BOOST_REQUIRE(!first.empty());
	const auto before = files_on_disk();

	std::vector<res::tag> second;
	import_and_export(second);
	BOOST_TEST(second.empty());
	BOOST_TEST((files_on_disk() == before));

	const auto material = std::ranges::find_if(first, [](const res::tag& tag) { return tag.view().ends_with(".mat.desc"); });
	BOOST_REQUIRE(material != first.end());
	BOOST_REQUIRE(rs->store_sync(*material, test_util::to_bytes("{}")));
	std::vector<res::tag> third;
	import_and_export(third);
	BOOST_TEST(third.size() == 1u);
	BOOST_TEST((files_on_disk() == before));
}

// EPIC-79 (#1068): the in-process import keeps the geometry in a binary file in memory://, the export writes the pair, and a
// mesh an old project kept in JSON arrays is converted at the next export, with the warning.
BOOST_AUTO_TEST_CASE(TheGeometry_IsExportedWithItsBinaryFile_AndAnOldArrayFileIsConverted)
{
	std::vector<res::tag> written;
	import_and_export(written);
	const auto geometry = std::ranges::find_if(exported, [](const res::tag& tag) { return tag.view().ends_with(".geom.desc"); });
	BOOST_REQUIRE(geometry != exported.end());
	const res::tag desc_tag = *geometry;
	const res::tag binary_tag = *res::desc_binary::binary_tag(desc_tag);
	BOOST_TEST(std::ranges::find(written, binary_tag) != written.end());
	BOOST_TEST(rs->exists(binary_tag));
	BOOST_TEST(!rs->exists(*res::desc_binary::staged_tag(desc_tag)));

	const res::tag memory_tag{ res::tag::memory, "tri/tri.geom.desc" };
	BOOST_TEST(rs->exists(*res::desc_binary::binary_tag(memory_tag)));
	const rnd::geometry_desc imported = geometry_of(memory_tag);
	const rnd::geometry_desc on_disk = geometry_of(desc_tag);
	BOOST_TEST(!imported.vertices.empty());
	BOOST_TEST((on_disk.vertices == imported.vertices));
	BOOST_TEST((on_disk.indices == imported.indices));
	BOOST_TEST(on_disk.layout.get_stride() == imported.layout.get_stride());

	// The project's file in the old form: written again as the pair, with one warning naming it.
	json::object arrays;
	rnd::geometry_desc plain;
	plain.layout = on_disk.layout;
	plain.vertices = on_disk.vertices;
	plain.indices = on_disk.indices;
	plain.serialize(arrays);
	BOOST_REQUIRE(rs->store_sync(desc_tag, test_util::to_bytes(json::serialize(arrays))));
	std::mutex said_mutex;
	std::vector<std::string> warned;
	const auto sink = engine::add_log_sink([&](std::string_view category, engine::log_level level, std::string_view message) {
		if (category != "Editor/Save" || level != engine::log_level::warn) return;
		std::lock_guard lock(said_mutex);
		warned.emplace_back(message);
	});
	std::vector<res::tag> again;
	import_and_export(again);
	BOOST_TEST(std::ranges::find(again, desc_tag) != again.end());
	BOOST_TEST(!rnd::holds_geometry_arrays(json::parse(files_on_disk().at(desc_tag.string())).as_object()));
	std::lock_guard lock(said_mutex);
	BOOST_REQUIRE(warned.size() == 1u);
	BOOST_TEST(warned.front().find(std::string(desc_tag.view())) != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
