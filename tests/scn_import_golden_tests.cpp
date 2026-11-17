#include <boost/test/unit_test.hpp>
#if SNK_HAS_ASSIMP
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_async_import_task.h"
#include "edt_import_problems.h"
#include "edt_model_importer.h"
#include <snk/common/hash/sha256.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/model_import/mdl_model_adapter.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// #1108: what each of the two model importers (the engine's adapter, the editor's import) writes for a set of models,
// against snapshots committed in tests/data/import_golden. Merging the importers keeps these snapshots; a difference the
// merge makes on purpose changes its snapshot in the same PR. What a snapshot holds: the memory:// files the import wrote
// and, for the editor, the import's result (tags, prefab JSON, skin weights); the instance the editor makes from it later
// on the main thread is not part of it.
namespace {

namespace json = boost::json;
namespace fs = std::filesystem;

constexpr std::string_view OBJ_LINES = "v 0 0 0\nv 1 0 0\nv 1 1 0\no Wire\nl 1 2\nl 2 3\n";

constexpr std::string_view OBJ_POINTS = "v 0 0 0\nv 1 0 0\nv 1 1 0\no Dots\np 1\np 2\np 3\n";

// A triangle, a line and a point in one object: only the triangle is kept.
constexpr std::string_view OBJ_MIXED = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\no Mixed\nf 1//1 2//1 3//1\nl 1 2\np 3\n";

constexpr std::string_view OBJ_NO_UV = "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nvn 0 0 1\no Bare\nf 1//1 2//1 3//1\n";

// Two objects, two materials: one with a texture beside the model, one with a colour only.
constexpr std::string_view OBJ_MULTI = R"(mtllib multi.mtl
v -1 -1 0
v 1 -1 0
v 0 1 0
v -1 -1 1
v 1 -1 1
v 0 1 1
vt 0 0
vt 1 0
vt 0.5 1
vn 0 0 1
o Textured
usemtl skin
f 1/1/1 2/2/1 3/3/1
o Plain
usemtl red
f 4//1 5//1 6//1
)";

constexpr std::string_view MTL_MULTI = "newmtl skin\nKd 1 1 1\nmap_Kd skin.png\nnewmtl red\nKd 1 0 0\nNs 32\n";

// What the importers read of a texture is its bytes: a PNG signature is enough.
const std::string PNG_BYTES = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(24, 'p');

// One triangle under a parent node with a translation animation; its buffer is the .bin file next to it.
constexpr std::string_view GLTF_WITH_BIN = R"GLTF({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Parent","translation":[1,2,3],"rotation":[0,0.7071068,0,0.7071068],"scale":[2,2,2],"children":[1]},{"name":"Child","mesh":0,"translation":[0,5,0]}],"meshes":[{"name":"TriMesh","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],"materials":[{"name":"Red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],"animations":[{"name":"Move","samplers":[{"input":4,"output":5,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}],"buffers":[{"byteLength":136,"uri":"tri.bin"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6},{"buffer":0,"byteOffset":104,"byteLength":8},{"buffer":0,"byteOffset":112,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]},{"bufferView":5,"componentType":5126,"count":2,"type":"VEC3"}]})GLTF";

constexpr unsigned char GLTF_BIN[] = {
	0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x41,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct model_file
{
	std::string name;
	std::string bytes;
};

// A model: the file imported first, the files beside it after.
struct golden_model
{
	std::string name;
	std::vector<model_file> files;
};

std::string read_binary(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::vector<golden_model> golden_models()
{
	return {
		{ "obj_lines", { { "wire.obj", std::string(OBJ_LINES) } } },
		{ "obj_points", { { "dots.obj", std::string(OBJ_POINTS) } } },
		{ "obj_mixed", { { "mixed.obj", std::string(OBJ_MIXED) } } },
		{ "obj_no_uv", { { "bare.obj", std::string(OBJ_NO_UV) } } },
		{ "obj_multi_mesh", { { "multi.obj", std::string(OBJ_MULTI) }, { "multi.mtl", std::string(MTL_MULTI) }, { "skin.png", PNG_BYTES } } },
		{ "gltf_with_bin", { { "tri.gltf", std::string(GLTF_WITH_BIN) },
			{ "tri.bin", std::string(reinterpret_cast<const char*>(GLTF_BIN), sizeof(GLTF_BIN)) } } },
		// Skinning, an animation, an embedded texture and its material.
		{ "gen_robot", { { "gen_robot.glb", read_binary(fs::path(SNK_TEST_PROJECT_DIR) / "res/objects/robot/gen_robot.glb") } } },
	};
}

std::span<const std::byte> as_bytes(const std::vector<std::byte>& data)
{
	return { data.data(), data.size() };
}

// Float arrays and long strings as figures that survive the last bits a compiler or assimp build changes: the snapshot
// of gen_robot stays small, and an array that changes shows in its count, sum, extremes or probe.
constexpr std::size_t k_kept_array = 32;
constexpr std::size_t k_kept_string = 256;

// A fixed weight in [-1, 1) per position (splitmix64 of the index). The probe, the sum of value times weight, moves by
// about a value times the square root of the count when values change places or turn their sign, while the noise of the
// last bits moves it by far less than its tolerance.
double probe_weight(std::size_t index)
{
	std::uint64_t z = static_cast<std::uint64_t>(index) + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	z ^= z >> 31;
	return static_cast<double>(z >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

json::object figures_of(std::span<const double> values)
{
	double sum = 0, magnitude = 0, probe = 0, probe_magnitude = 0;
	double low = values.empty() ? 0 : values.front(), high = low;
	for (std::size_t i = 0; i < values.size(); ++i) {
		const double value = values[i];
		sum += value;
		magnitude += std::abs(value);
		probe += probe_weight(i) * value;
		probe_magnitude += std::abs(probe_weight(i) * value);
		low = std::min(low, value);
		high = std::max(high, value);
	}
	return { { "count", values.size() }, { "sum", sum }, { "magnitude", magnitude }, { "probe", probe },
		{ "probe_magnitude", probe_magnitude }, { "min", low }, { "max", high } };
}

json::value summarized(const json::value& value)
{
	if (value.is_object()) {
		json::object out;
		for (const auto& [key, item] : value.as_object()) {
			// The hash of a range of the .geom.bin: the bytes of floats, kept out like the bytes themselves.
			if (key == "sha256") continue;
			out[key] = summarized(item);
		}
		return out;
	}
	if (value.is_array()) {
		const json::array& items = value.as_array();
		const bool numbers = !items.empty() && std::all_of(items.begin(), items.end(), [](const json::value& item) { return item.is_number(); });
		if (numbers && items.size() > k_kept_array) {
			std::vector<double> values;
			values.reserve(items.size());
			for (const auto& item : items) values.push_back(item.to_number<double>());
			return json::object{ { "numbers", figures_of(values) } };
		}
		// Rows of numbers (the skin weights of each vertex): the figures of each column.
		const bool rows = items.size() > k_kept_array && std::all_of(items.begin(), items.end(), [](const json::value& item) {
			return item.is_array() && std::all_of(item.as_array().begin(), item.as_array().end(), [](const json::value& cell) { return cell.is_number(); });
		});
		if (rows) {
			std::vector<std::vector<double>> columns;
			for (const auto& row : items) {
				const json::array& cells = row.as_array();
				if (columns.size() < cells.size()) columns.resize(cells.size());
				for (std::size_t c = 0; c < cells.size(); ++c) columns[c].push_back(cells[c].to_number<double>());
			}
			json::array figures;
			for (const auto& column : columns) figures.push_back(figures_of(column));
			return json::object{ { "rows", json::object{ { "count", items.size() }, { "columns", std::move(figures) } } } };
		}
		json::array out;
		out.reserve(items.size());
		for (const auto& item : items) out.push_back(summarized(item));
		return out;
	}
	if (value.is_string() && value.as_string().size() > k_kept_string) {
		const std::string_view text = value.as_string();
		return json::object{ { "string", json::object{ { "size", text.size() },
			{ "sha256", cmn::sha256_hex(std::as_bytes(std::span(text.data(), text.size()))) } } } };
	}
	return value;
}

// The geometry as the loader reads it: its layout, its indices exactly, the figures of each vertex element.
json::object decoded_geometry(res::resource_system& system, const res::tag& tag, const json::object& desc)
{
	rnd::geometry_desc geometry;
	const std::string error = rnd::read_geometry_file(system, tag, desc, geometry);
	if (!error.empty()) return { { "error", error } };

	json::object out;
	const std::size_t stride = geometry.layout.get_stride();
	out["topology"] = rnd::topology_name(rnd::topology_of(desc));
	out["stride"] = stride;
	const std::size_t vertices = stride == 0 ? 0 : geometry.vertices.size() / stride;
	out["vertices"] = vertices;
	json::array indices;
	if (geometry.indices.size() <= k_kept_array) {
		for (const unsigned index : geometry.indices) indices.push_back(index);
		out["indices"] = std::move(indices);
	} else {
		const auto bytes = std::as_bytes(std::span(geometry.indices));
		out["indices"] = json::object{ { "count", geometry.indices.size() }, { "sha256", cmn::sha256_hex(bytes) } };
	}

	// A small geometry whole, vertex by vertex; a big one as the figures of each component.
	json::object elements;
	for (const auto& element : geometry.layout) {
		const std::size_t floats = element.Size / sizeof(float);
		std::vector<std::vector<double>> components(floats);
		json::array whole;
		for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
			json::array values;
			for (std::size_t c = 0; c < floats; ++c) {
				float value = 0;
				std::memcpy(&value, geometry.vertices.data() + vertex * stride + element.Offset + c * sizeof(float), sizeof(value));
				components[c].push_back(value);
				values.push_back(static_cast<double>(value));
			}
			whole.push_back(std::move(values));
		}
		if (vertices <= k_kept_array) {
			elements[element.Name] = std::move(whole);
			continue;
		}
		json::array figures;
		for (const auto& component : components) figures.push_back(figures_of(component));
		elements[element.Name] = std::move(figures);
	}
	out["elements"] = std::move(elements);
	return out;
}

// Every memory:// file an import wrote, but the model's own files: JSON as its summary (a geometry desc also as the
// geometry it holds), anything else as its size and hash. A .geom.bin is the geometry of its desc.
json::object snapshot_of(res::resource_system& system, const std::unordered_map<res::tag, std::vector<std::byte>>& files,
	const std::vector<std::string>& inputs)
{
	std::map<std::string, const std::vector<std::byte>*> sorted;
	for (const auto& [tag, bytes] : files) {
		const std::string name(tag.view());
		if (std::find(inputs.begin(), inputs.end(), name) == inputs.end()) sorted.emplace(name, &bytes);
	}

	json::object out;
	for (const auto& [name, bytes] : sorted) {
		if (name.ends_with(".geom.bin")) {
			out[name] = json::object{ { "binary", bytes->size() } };
			continue;
		}
		boost::system::error_code ec;
		const json::value parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), ec);
		if (ec || !parsed.is_object()) {
			out[name] = json::object{ { "size", bytes->size() }, { "sha256", cmn::sha256_hex(as_bytes(*bytes)) } };
			continue;
		}
		json::object entry{ { "json", summarized(parsed) } };
		if (name.ends_with(".geom.desc")) entry["geometry"] = decoded_geometry(system, res::tag{ name }, parsed.as_object());
		out[name] = std::move(entry);
	}
	return out;
}

bool is_integer(const json::value& value)
{
	return value.is_int64() || value.is_uint64();
}

// Integers (counts, offsets, indices) exactly. Other numbers within 1e-4 of the larger magnitude; a sum within 1e-4 of
// the magnitude of what was summed, which a sum close to zero needs; a probe within 1e-6 of its magnitude. Object keys in
// any order. The first difference, or nothing.
std::string difference(const json::value& expected, const json::value& found, const std::string& where, double scale = 0)
{
	if (is_integer(expected) && is_integer(found)) {
		return expected.to_number<double>() == found.to_number<double>()
			? std::string() : where + ": " + json::serialize(expected) + " != " + json::serialize(found);
	}
	if (expected.is_number() && found.is_number()) {
		const double a = expected.to_number<double>(), b = found.to_number<double>();
		const double tolerance = 1e-4 * std::max({ 1.0, std::abs(a), std::abs(b), scale });
		return std::abs(a - b) <= tolerance ? std::string() : where + ": " + json::serialize(expected) + " != " + json::serialize(found);
	}
	if (expected.kind() != found.kind()) return where + ": " + json::serialize(expected) + " != " + json::serialize(found);
	if (expected.is_object()) {
		const json::object& a = expected.as_object();
		const json::object& b = found.as_object();
		for (const auto& [key, item] : a) {
			if (!b.contains(key)) return where + "/" + std::string(key) + ": missing";
		}
		for (const auto& [key, item] : b) {
			if (!a.contains(key)) return where + "/" + std::string(key) + ": not in the snapshot";
		}
		const auto number_at = [&](std::string_view key) { return a.contains(key) && a.at(key).is_number() ? a.at(key).to_number<double>() : 0.0; };
		const double magnitude = number_at("magnitude");
		const double probe_magnitude = number_at("probe_magnitude");
		for (const auto& [key, item] : a) {
			const double nested = key == "sum" ? magnitude : key == "probe" ? 0.01 * probe_magnitude : 0;
			if (auto found_one = difference(item, b.at(key), where + "/" + std::string(key), nested); !found_one.empty()) return found_one;
		}
		return {};
	}
	if (expected.is_array()) {
		const json::array& a = expected.as_array();
		const json::array& b = found.as_array();
		if (a.size() != b.size()) return where + ": " + std::to_string(a.size()) + " items != " + std::to_string(b.size());
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (auto found_one = difference(a[i], b[i], where + "[" + std::to_string(i) + "]"); !found_one.empty()) return found_one;
		}
		return {};
	}
	return expected == found ? std::string() : where + ": " + json::serialize(expected) + " != " + json::serialize(found);
}

// Indented, keys as written: the snapshot is reviewed as a diff.
void write_pretty(std::ostream& out, const json::value& value, int depth = 0)
{
	const std::string indent(static_cast<std::size_t>(depth + 1), '\t');
	const std::string close(static_cast<std::size_t>(depth), '\t');
	if (value.is_object() && !value.as_object().empty()) {
		out << "{\n";
		bool first = true;
		for (const auto& [key, item] : value.as_object()) {
			out << (first ? "" : ",\n") << indent << json::serialize(json::string(key)) << ": ";
			write_pretty(out, item, depth + 1);
			first = false;
		}
		out << "\n" << close << "}";
	} else if (value.is_array() && !value.as_array().empty()) {
		out << "[\n";
		bool first = true;
		for (const auto& item : value.as_array()) {
			out << (first ? "" : ",\n") << indent;
			write_pretty(out, item, depth + 1);
			first = false;
		}
		out << "\n" << close << "]";
	} else {
		out << json::serialize(value);
	}
}

// A snapshot that is missing or differs is written to generated_import_golden/ next to the test binary: the Linux Clang
// job of CI uploads that folder as an artifact, and its files go into tests/data/import_golden unchanged.
void check_snapshot(const std::string& file_name, const json::object& found)
{
	const fs::path committed = fs::path(SNK_TEST_FIXTURES_DIR).parent_path() / "import_golden" / file_name;
	std::string problem;
	std::ifstream in(committed, std::ios::binary);
	if (!in) {
		problem = "no snapshot";
	} else {
		boost::system::error_code ec;
		const json::value expected = json::parse(std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() }, ec);
		problem = ec ? "the snapshot is not JSON: " + ec.message() : difference(expected, found, "");
	}
	if (problem.empty()) return;

	const fs::path generated = fs::path("generated_import_golden") / file_name;
	fs::create_directories(generated.parent_path());
	std::ofstream out(generated, std::ios::binary);
	write_pretty(out, found);
	out << "\n";
	BOOST_ERROR("tests/data/import_golden/" + file_name + ": " + problem + " (the import's snapshot is in the generated_import_golden artifact)");
}

json::array sorted_tags(const std::vector<res::tag>& tags)
{
	std::vector<std::string> names;
	for (const res::tag& tag : tags) names.emplace_back(tag.view());
	std::sort(names.begin(), names.end());
	json::array out;
	for (const std::string& name : names) out.push_back(json::string(name));
	return out;
}

// What the editor's import hands on besides the files: the main thread makes the prefab and its skin weights from it.
json::object result_of(const edt::import_intermediate& result)
{
	json::array weights;
	for (const auto& row : result.skin_weights) {
		json::array cells;
		for (const std::uint32_t cell : row) cells.push_back(cell);
		weights.push_back(std::move(cells));
	}
	return { { "geom_tag", result.geom_tag.view() }, { "prefab_tag", result.prefab_tag.view() },
		{ "all_tags", sorted_tags(result.all_tags) }, { "created_tags", sorted_tags(result.created_tags) },
		{ "prefab_json", summarized(result.prefab_json) }, { "skin_weights", summarized(weights) } };
}

struct golden_fixture : test_util::scene_env
{
	golden_fixture()
	{
		rs->registrate_adapter<mdl::model_adapter>(mdl::model_adapter::INFO, std::ref(*descs), std::ref(*rs));
	}

	// Engine importer: the model and its side files in memory://models/<model>/, loaded as a prefab.
	json::object engine_snapshot(const golden_model& model)
	{
		std::vector<std::string> inputs;
		for (const model_file& file : model.files) {
			const res::tag tag{ "memory://models/" + model.name + "/" + file.name };
			rs->store(tag, test_util::to_bytes(file.bytes));
			inputs.emplace_back(tag.view());
		}
		const auto prefab = require_loaded<scn::prefab_desc>(res::tag{ inputs.front() }).get_sync();
		BOOST_TEST_REQUIRE(static_cast<bool>(prefab));
		return snapshot_of(*rs, ram->files, inputs);
	}

	// Editor importer, in this process: the model and its side files in a scratch folder.
	json::object editor_snapshot(const golden_model& model)
	{
		test_util::temp_dir dir{ "scn_import_golden_" + model.name };
		for (const model_file& file : model.files) dir.write(file.name, file.bytes);
		edt::model_importer importer(*descs, *rs);
		edt::import_source source;
		source.path = dir.path() / model.files.front().name;
		const edt::import_intermediate result = importer.import_background(source);
		BOOST_TEST_REQUIRE(result.error.empty(), result.error);
		json::object out = snapshot_of(*rs, ram->files, {});
		// Not a memory:// tag, so it can name no file.
		out["(import result)"] = result_of(result);
		return out;
	}
};

}

BOOST_AUTO_TEST_SUITE(ScnImportGoldenTests)

BOOST_AUTO_TEST_CASE(TheEngineImporter_WritesTheSnapshotOfEachModel)
{
	for (const golden_model& model : golden_models()) {
		BOOST_TEST_CONTEXT(model.name) {
			golden_fixture env;
			check_snapshot(model.name + ".engine.json", env.engine_snapshot(model));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheEditorImporter_WritesTheSnapshotOfEachModel)
{
	for (const golden_model& model : golden_models()) {
		BOOST_TEST_CONTEXT(model.name) {
			golden_fixture env;
			check_snapshot(model.name + ".editor.json", env.editor_snapshot(model));
		}
	}
}

// #1108: a texture the model names that is not there is reported by the import (and, for a person, in Problems) and
// gets neither a desc nor a sampler; the editor's import once wrote both for a file it never read.
BOOST_AUTO_TEST_CASE(TheEditorImporter_ReportsAMissingTexture_AndGivesItNoSampler)
{
	golden_fixture env;
	test_util::temp_dir dir{ "scn_import_missing_texture" };
	dir.write("lost.mtl", "newmtl skin\nKd 1 1 1\nmap_Kd missing.png\n");
	dir.write("lost.obj", "mtllib lost.mtl\nv -1 -1 0\nv 1 -1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0.5 1\nvn 0 0 1\no Tri\nusemtl skin\nf 1/1/1 2/2/1 3/3/1\n");
	edt::model_importer importer(*env.descs, *env.rs);
	edt::import_source source;
	source.path = dir.path() / "lost.obj";
	const edt::import_intermediate result = importer.import_background(source);
	BOOST_TEST_REQUIRE(result.error.empty(), result.error);
	BOOST_TEST((result.missing_textures == std::vector<std::string>{ "missing.png" }));

	const auto material = env.ram->files.find(res::tag{ "memory://lost/materials/skin.mat.desc" });
	BOOST_REQUIRE(material != env.ram->files.end());
	const json::object desc = json::parse(std::string_view(reinterpret_cast<const char*>(material->second.data()), material->second.size())).as_object();
	BOOST_TEST(!desc.contains("samplers"));
	BOOST_TEST(!env.rs->exists(res::tag{ "memory://lost/textures/missing.png.txm.desc" }));

	const std::vector<edt::problem> problems = edt::import_problems(result, "lost.obj");
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST((problems.front().severity == edt::problem_severity::warning));
	BOOST_TEST(problems.front().message.find("missing.png") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheComparison_AcceptsFloatNoise_AndRefusesAChange)
{
	const json::value snapshot = json::parse(R"({"a":[1,2,3],"b":{"count":4,"sum":0.0,"magnitude":1000.0,"min":-1.0,"max":1.0},"c":"x"})");
	BOOST_TEST(difference(snapshot, snapshot, "").empty());
	BOOST_TEST(difference(snapshot, json::parse(R"({"c":"x","a":[1,2,3.00001],"b":{"count":4,"sum":0.05,"magnitude":1000.0,"min":-1.0,"max":1.0}})"), "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"a":[1,2,3.01],"b":{"count":4,"sum":0.0,"magnitude":1000.0,"min":-1.0,"max":1.0},"c":"x"})"), "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"a":[1,2,3],"b":{"count":4,"sum":0.5,"magnitude":1000.0,"min":-1.0,"max":1.0},"c":"x"})"), "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"a":[1,2],"b":{"count":4,"sum":0.0,"magnitude":1000.0,"min":-1.0,"max":1.0},"c":"x"})"), "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"a":[1,2,3],"b":{"count":4,"sum":0.0,"magnitude":1000.0,"min":-1.0,"max":1.0},"c":"y"})"), "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"a":[1,2,3],"b":{"count":4,"sum":0.0,"magnitude":1000.0,"min":-1.0,"max":1.0}})"), "").empty());
}

BOOST_AUTO_TEST_CASE(TheComparison_TakesIntegersExactly)
{
	const json::value snapshot = json::parse(R"({"vx_end":42686,"count":3})");
	BOOST_TEST(difference(snapshot, snapshot, "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"vx_end":42687,"count":3})"), "").empty());
	BOOST_TEST(!difference(snapshot, json::parse(R"({"vx_end":42686,"count":4})"), "").empty());
}

// The figures of a big array: noise in the last bits passes; a value moved to the next vertex, two neighbours swapped,
// or normals turned over do not, even when the count, the sum and the extremes stay the same.
BOOST_AUTO_TEST_CASE(TheFigures_AcceptNoise_AndRefuseMovedOrTurnedValues)
{
	std::vector<double> values(42686);
	for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::sin(0.37 * static_cast<double>(i));
	const json::value expected = figures_of(values);

	std::vector<double> noisy = values;
	for (std::size_t i = 0; i < noisy.size(); ++i) noisy[i] *= 1.0 + (i % 2 == 0 ? 1e-6 : -1e-6);
	BOOST_TEST(difference(expected, figures_of(noisy), "").empty());

	std::vector<double> moved = values;
	std::rotate(moved.begin(), moved.begin() + 1, moved.end());
	BOOST_TEST(!difference(expected, figures_of(moved), "").empty());

	std::vector<double> swapped = values;
	std::swap(swapped[1000], swapped[1001]);
	BOOST_TEST(!difference(expected, figures_of(swapped), "").empty());

	std::vector<double> turned = values;
	for (double& value : turned) value = -value;
	BOOST_TEST(!difference(expected, figures_of(turned), "").empty());
}

BOOST_AUTO_TEST_SUITE_END()

#endif
