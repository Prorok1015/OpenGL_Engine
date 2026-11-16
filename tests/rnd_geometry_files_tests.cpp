#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "geom/rnd_geom_manager.h"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_vfs_test_hook.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#if SNK_HAS_ASSIMP
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// EPIC-79 PR3a (#1068): every writer of a geometry desc writes it with its binary file, through rnd::write_geometry_files,
// and never a file the loader refuses.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

// `count` triangles over 3 * `count` vertices of a position and a uv each.
rnd::geometry_desc triangles(int count)
{
	rnd::geometry_desc out;
	out.layout = rnd::driver::BufferLayout{ { rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" },
		{ rnd::driver::SHADER_DATA_TYPE::VEC2_F, "uv" } };
	std::vector<float> floats;
	for (int v = 0; v < 3 * count; ++v) {
		const float f = static_cast<float>(v);
		floats.insert(floats.end(), { f, f + 0.5f, -f, f / 8.0f, 1.0f - f / 8.0f });
		out.indices.push_back(static_cast<unsigned int>(v));
	}
	out.vertices.resize(floats.size() * sizeof(float));
	std::memcpy(out.vertices.data(), floats.data(), out.vertices.size());
	return out;
}

std::string text_of(const std::vector<std::byte>& bytes)
{
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool mentions(const std::string& text, std::string_view part)
{
	return text.find(part) != std::string::npos;
}

struct geometry_files_fixture : test_util::null_engine_fixture
{
	geometry_files_fixture() : null_engine_fixture(false)
	{
		// A project layer on disk: the save goes through a staged file and a commit (res::desc_binary::save).
		fs::create_directories(dir.path() / "a");
		resolver = &root().registrate_resolver<res::vfs_resolver>("geomfiles", std::vector<fs::path>{ dir.path() });
	}

	~geometry_files_fixture()
	{
		root().unregistrate_resolver("geomfiles");
	}

	res::resource_system& root() { return storage.require<res::resource_system>(); }

	template<class T>
	res::res_handle<T> load(const res::tag& tag)
	{
		auto handle = root().require_sync<T>(tag);
		test_util::pump(tasks);
		return handle;
	}

	json::object desc_on_disk(std::string_view name) const
	{
		std::ifstream in(dir.path() / "a" / name, std::ios::binary);
		const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		return json::parse(text).as_object();
	}

	std::vector<std::byte> bytes_on_disk(std::string_view name) const
	{
		std::ifstream in(dir.path() / "a" / name, std::ios::binary);
		const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		std::vector<std::byte> out(text.size());
		std::memcpy(out.data(), text.data(), text.size());
		return out;
	}

	// The next `count` writes of the file `name` (in a/) fail on the io thread; `name` and nothing longer: a/<name>.new
	// is another file.
	void fail_writes_of(std::string_view name, int count)
	{
		failures = count;
		const std::string prefix = "." + std::string(name) + ".";
		res::detail::write_hook_access::set(*resolver, [this, prefix](res::detail::write_stage stage, const fs::path& tmp) {
			const std::string leaf = tmp.filename().string();
			if (stage != res::detail::write_stage::after_check || !leaf.starts_with(prefix) || leaf.starts_with(prefix + "new.")) {
				return;
			}
			if (failures.fetch_sub(1) > 0) {
				throw std::runtime_error("a write the test fails");
			}
		});
	}

	void stop_failing() { res::detail::write_hook_access::set(*resolver, nullptr); }

	test_util::temp_dir dir{ "rnd_geometry_files" };
	res::vfs_resolver* resolver = nullptr;
	std::atomic<int> failures{ 0 };
};

}

BOOST_FIXTURE_TEST_SUITE(RndGeometryFilesTests, geometry_files_fixture)

BOOST_AUTO_TEST_CASE(AWrittenGeometry_LoadsBackIdentically_FromItsBinaryFile)
{
	const res::tag tag{ "geomfiles://a/mesh.geom.desc" };
	const rnd::geometry_desc written = triangles(4);
	rnd::geometry_file_options options;
	options.header["__parent"] = "res://base_geometry.desc";

	const std::string error = rnd::write_geometry_files(root(), tag, written, options);
	BOOST_REQUIRE_MESSAGE(error.empty(), error);
	BOOST_TEST(fs::exists(dir.path() / "a" / "mesh.geom.bin"));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "mesh.geom.bin.new"));

	const json::object desc = desc_on_disk("mesh.geom.desc");
	BOOST_TEST(desc.at("__version").as_int64() == rnd::geometry_binary_version);
	BOOST_TEST(std::string(desc.at("__parent").as_string()) == "res://base_geometry.desc");
	BOOST_TEST(std::string(desc.at("vertices").at("layout").as_string()) == "f32x5");
	BOOST_TEST(std::string(desc.at("indices").at("layout").as_string()) == "u32");
	BOOST_TEST(!desc.contains("bounds"));
	BOOST_TEST(!rnd::holds_geometry_arrays(desc));

	const auto loaded = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	BOOST_TEST((loaded->vertices == written.vertices));
	BOOST_TEST((loaded->indices == written.indices));
	BOOST_TEST(loaded->layout.get_stride() == written.layout.get_stride());
	BOOST_TEST(loaded->bounds.empty());
}

BOOST_AUTO_TEST_CASE(AnAttributeThatIsNoFloat_MakesTheVerticesBytes)
{
	const res::tag tag{ "geomfiles://a/ints.geom.desc" };
	rnd::geometry_desc geometry = triangles(1);
	geometry.layout = rnd::driver::BufferLayout{ { rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" },
		{ rnd::driver::SHADER_DATA_TYPE::VEC2_I, "ids" } };

	const rnd::geometry_files files = rnd::encode_geometry_files(tag, geometry);
	BOOST_REQUIRE_MESSAGE(files, files.error);
	const json::object desc = json::parse(text_of(files.desc)).as_object();
	BOOST_TEST(std::string(desc.at("vertices").at("layout").as_string()) == "u8");

	BOOST_REQUIRE(rnd::write_geometry_files(root(), tag, geometry).empty());
	const auto loaded = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	BOOST_TEST((loaded->vertices == geometry.vertices));
}

BOOST_AUTO_TEST_CASE(Bounds_AreAKeyOnly_AndTheLoaderBuildsThemFromTheData)
{
	const res::tag tag{ "geomfiles://a/bounds.geom.desc" };
	rnd::geometry_file_options options;
	options.bounds = true;
	BOOST_REQUIRE(rnd::write_geometry_files(root(), tag, triangles(3), options).empty());

	BOOST_TEST(desc_on_disk("bounds.geom.desc").at("bounds").as_array().empty());
	const auto loaded = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	BOOST_TEST(loaded->bounds.size() == 3u);
}

BOOST_AUTO_TEST_CASE(TheHeader_KeepsItsKeys_ButNeverTheWritersOwn)
{
	rnd::geometry_file_options options;
	options.header["__guid"] = "ad31206f-54a5-4892-9e60-5b138b5b6af4";
	options.header["__version"] = 7;
	options.header["vertices"] = json::array{ 1, 2, 3 };
	const rnd::geometry_files files = rnd::encode_geometry_files(res::tag{ "geomfiles://a/h.geom.desc" }, triangles(1), options);
	BOOST_REQUIRE_MESSAGE(files, files.error);
	const json::object desc = json::parse(text_of(files.desc)).as_object();
	BOOST_TEST(std::string(desc.at("__guid").as_string()) == "ad31206f-54a5-4892-9e60-5b138b5b6af4");
	BOOST_TEST(desc.at("__version").as_int64() == rnd::geometry_binary_version);
	BOOST_TEST(desc.at("vertices").is_object());
	BOOST_TEST(std::ranges::count(text_of(files.desc), '\n') == 0);
}

BOOST_AUTO_TEST_CASE(DataTheLoaderRefuses_IsNeverWritten)
{
	rnd::geometry_desc past = triangles(1);
	past.indices[1] = 3;
	const std::string past_error = rnd::write_geometry_files(root(), res::tag{ "geomfiles://a/past.geom.desc" }, past);
	BOOST_TEST(mentions(past_error, "/indices"));

	rnd::geometry_desc nan = triangles(1);
	const float bad = std::numeric_limits<float>::quiet_NaN();
	std::memcpy(nan.vertices.data(), &bad, sizeof(bad));
	BOOST_TEST(mentions(rnd::write_geometry_files(root(), res::tag{ "geomfiles://a/nan.geom.desc" }, nan), "/vertices"));

	rnd::geometry_desc empty = triangles(1);
	empty.vertices.clear();
	empty.indices.clear();
	BOOST_TEST(mentions(rnd::write_geometry_files(root(), res::tag{ "geomfiles://a/empty.geom.desc" }, empty), "no vertices"));

	BOOST_TEST(!rnd::write_geometry_files(root(), res::tag{ "geomfiles://a/plain.desc" }, triangles(1)).empty());

	for (const char* name : { "past.geom.desc", "past.geom.bin", "past.geom.bin.new", "nan.geom.desc", "empty.geom.desc", "plain.desc", "plain.bin" }) {
		BOOST_TEST_CONTEXT(name) { BOOST_TEST(!fs::exists(dir.path() / "a" / name)); }
	}
}

BOOST_AUTO_TEST_CASE(AFileIsReadWithoutTheCache_ArraysAsTheyAre_RangesWithTheirHashes)
{
	const rnd::geometry_desc source = triangles(2);
	json::object arrays;
	source.serialize(arrays);
	rnd::geometry_desc from_arrays;
	BOOST_REQUIRE(rnd::read_geometry_file(root(), res::tag{ "geomfiles://a/arrays.geom.desc" }, arrays, from_arrays).empty());
	BOOST_TEST((from_arrays.vertices == source.vertices));
	BOOST_TEST(rnd::holds_geometry_arrays(arrays));

	const res::tag tag{ "geomfiles://a/ranges.geom.desc" };
	BOOST_REQUIRE(rnd::write_geometry_files(root(), tag, source).empty());
	const json::object ranges = desc_on_disk("ranges.geom.desc");
	rnd::geometry_desc from_ranges;
	BOOST_REQUIRE(rnd::read_geometry_file(root(), tag, ranges, from_ranges).empty());
	BOOST_TEST((from_ranges.vertices == source.vertices));
	BOOST_TEST((from_ranges.indices == source.indices));

	// Another binary file of the same size: the hashes refuse it.
	rnd::geometry_desc other = triangles(2);
	other.vertices[4] = std::byte{ 0x7f };
	const rnd::geometry_files changed = rnd::encode_geometry_files(tag, other);
	BOOST_REQUIRE(changed);
	{
		std::ofstream out(dir.path() / "a" / "ranges.geom.bin", std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(changed.binary.data()), static_cast<std::streamsize>(changed.binary.size()));
	}
	rnd::geometry_desc tampered;
	BOOST_TEST(!rnd::read_geometry_file(root(), tag, ranges, tampered).empty());
}

BOOST_AUTO_TEST_CASE(AGenericStoreOfAGeometry_IsRefused_AndWritesNothing)
{
	std::mutex said_mutex;
	bool refused = false;
	const auto sink = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view message) {
		if (message.find("pinned.geom.desc") == std::string_view::npos) return;
		std::lock_guard lock(said_mutex);
		refused = refused || message.find("write_geometry_files") != std::string_view::npos;
	});
	const res::tag tag{ "memory://geometry_files/pinned.geom.desc" };
	root().pin_resource(tag, triangles(1));
	for (int i = 0; i < 4; ++i) {
		test_util::pump(tasks);
	}
	{
		std::lock_guard lock(said_mutex);
		BOOST_TEST(refused);
	}
	BOOST_TEST(!root().exists(tag));
}

BOOST_AUTO_TEST_CASE(AGeometryWrittenAgain_IsDrawnByTheGeomManager_WithoutASignalOfTheCaller)
{
	// geom_manager draws memory:// but not geomfiles://, and the fixture's memory:// keeps files in RAM, where nothing commits.
	fs::create_directories(dir.path() / "memory");
	root().unregistrate_resolver(res::tag::memory);
	root().registrate_resolver<res::vfs_resolver>(res::tag::memory, std::vector<fs::path>{ dir.path() / "memory" });
	const res::tag tag{ "memory://redrawn.geom.desc" };
	const std::string first_error = rnd::write_geometry_files(root(), tag, triangles(1));
	BOOST_REQUIRE_MESSAGE(first_error.empty(), first_error);
	rnd::geom_manager geometry(storage.require<rnd::render_system>().get_driver(), root());
	const auto frame = [&] {
		test_util::pump(tasks);
		geometry.begin_frame();
		return geometry.require_geometry(tag);
	};
	// The disk mount loads on its io thread: frames go on until the buffers exist, within a time limit.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	const auto in_time = [&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return std::chrono::steady_clock::now() < deadline;
	};
	rnd::driver::vertex_array_interface* first = nullptr;
	while (!(first = frame()) && in_time()) {}
	BOOST_REQUIRE(first != nullptr);
	BOOST_TEST(!geometry.draw_fits(tag, 6, 0, 0));

	// The writer itself tells the cache: the test signals nothing.
	const std::string error = rnd::write_geometry_files(root(), tag, triangles(2));
	BOOST_REQUIRE_MESSAGE(error.empty(), error);
	while (geometry.require_geometry(tag) == first && in_time()) {
		BOOST_REQUIRE(frame() != nullptr);
	}
	BOOST_TEST(geometry.draw_fits(tag, 6, 0, 0));
}

// The review of #1091: a commit that fails after the desc was written never leaves a desc without its binary file.
BOOST_AUTO_TEST_CASE(ACommitThatFailsOnce_IsTriedAgain_AndThePairIsWhole)
{
	const res::tag tag{ "geomfiles://a/retry.geom.desc" };
	fail_writes_of("retry.geom.bin", 1);
	const std::string error = rnd::write_geometry_files(root(), tag, triangles(2));
	stop_failing();
	BOOST_REQUIRE_MESSAGE(error.empty(), error);
	BOOST_TEST(fs::exists(dir.path() / "a" / "retry.geom.bin"));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "retry.geom.bin.new"));
	const auto loaded = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	BOOST_TEST((loaded->indices == triangles(2).indices));
}

BOOST_AUTO_TEST_CASE(ACommitThatFailsTwice_TakesTheNewDescBack_AndThenItsStagedFile)
{
	// A new file: the desc goes, then the staged file; nothing is left to take back.
	const res::tag fresh{ "geomfiles://a/fresh.geom.desc" };
	std::vector<res::tag> left;
	fail_writes_of("fresh.geom.bin", 2);
	const std::string fresh_error = rnd::write_geometry_files(root(), fresh, triangles(1), {}, &left);
	stop_failing();
	BOOST_TEST(mentions(fresh_error, "not committed"));
	BOOST_TEST(mentions(fresh_error, "the desc was removed"));
	BOOST_TEST(left.empty());
	for (const char* name : { "fresh.geom.desc", "fresh.geom.bin", "fresh.geom.bin.new" }) {
		BOOST_TEST_CONTEXT(name) { BOOST_TEST(!fs::exists(dir.path() / "a" / name)); }
	}

	// Over a written pair: the previous desc comes back beside the previous binary file, which the commit never replaced.
	const res::tag tag{ "geomfiles://a/kept.geom.desc" };
	BOOST_REQUIRE(rnd::write_geometry_files(root(), tag, triangles(1)).empty());
	const auto old_desc = bytes_on_disk("kept.geom.desc");
	const auto old_binary = bytes_on_disk("kept.geom.bin");
	fail_writes_of("kept.geom.bin", 2);
	const std::string error = rnd::write_geometry_files(root(), tag, triangles(3), {}, &left);
	stop_failing();
	BOOST_TEST(mentions(error, "the previous desc was put back"));
	BOOST_TEST(left.empty());
	BOOST_TEST((bytes_on_disk("kept.geom.desc") == old_desc));
	BOOST_TEST((bytes_on_disk("kept.geom.bin") == old_binary));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "kept.geom.bin.new"));
	const auto loaded = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	BOOST_TEST((loaded->indices == triangles(1).indices));
}

BOOST_AUTO_TEST_CASE(ADescThatCannotBeTakenBack_KeepsItsStagedFile_AndNamesBoth)
{
	const res::tag tag{ "geomfiles://a/stuck.geom.desc" };
	BOOST_REQUIRE(rnd::write_geometry_files(root(), tag, triangles(1)).empty());
	// The binary file fails twice, so does the desc put back after the new one was written.
	std::atomic<int> desc_writes{ 0 };
	failures = 2;
	res::detail::write_hook_access::set(*resolver, [&](res::detail::write_stage stage, const fs::path& tmp) {
		if (stage != res::detail::write_stage::after_check) return;
		const std::string leaf = tmp.filename().string();
		if (leaf.starts_with(".stuck.geom.desc.") && desc_writes.fetch_add(1) >= 1) {
			throw std::runtime_error("a write the test fails");
		}
		if (leaf.starts_with(".stuck.geom.bin.") && !leaf.starts_with(".stuck.geom.bin.new.") && failures.fetch_sub(1) > 0) {
			throw std::runtime_error("a write the test fails");
		}
	});
	std::vector<res::tag> left;
	const std::string error = rnd::write_geometry_files(root(), tag, triangles(2), {}, &left);
	stop_failing();
	BOOST_TEST(mentions(error, "taking it back failed too"));
	const res::tag staged = *res::desc_binary::staged_tag(tag);
	BOOST_TEST((left == std::vector<res::tag>{ staged, tag }));
	// The new desc needs the staged file: it is there, and a reader takes the data from it.
	BOOST_TEST(fs::exists(dir.path() / "a" / "stuck.geom.bin.new"));
	rnd::geometry_desc read;
	const std::string read_error = rnd::read_geometry_file(root(), tag, desc_on_disk("stuck.geom.desc"), read);
	BOOST_REQUIRE_MESSAGE(read_error.empty(), read_error);
	BOOST_TEST((read.indices == triangles(2).indices));
}

// Topology is the caller's to say: only triangles get a binary file, the rest stays in arrays that name it.
BOOST_AUTO_TEST_CASE(OnlyTriangles_AreWrittenWithABinaryFile)
{
	rnd::geometry_file_options lines;
	lines.topology = rnd::geometry_topology::lines;
	const std::string error = rnd::write_geometry_files(root(), res::tag{ "geomfiles://a/lines.geom.desc" }, triangles(1), lines);
	BOOST_TEST(mentions(error, "not triangles"));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "lines.geom.desc"));
	BOOST_TEST(!rnd::encode_geometry_files(res::tag{ "geomfiles://a/lines.geom.desc" }, triangles(1), lines));

	BOOST_TEST((rnd::topology_of(json::object{}) == rnd::geometry_topology::triangles));
	BOOST_TEST((rnd::topology_of(json::object{ { "topology", "lines" } }) == rnd::geometry_topology::lines));
	BOOST_TEST((rnd::topology_of(json::object{ { "topology", "points" } }) == rnd::geometry_topology::points));
	BOOST_TEST((rnd::topology_of(json::object{ { "topology", "strips" } }) == rnd::geometry_topology::mixed));
	BOOST_TEST((rnd::topology_of(json::object{ { "topology", 3 } }) == rnd::geometry_topology::mixed));

	// The writer's own key: a header never carries it into a desc with ranges.
	rnd::geometry_file_options header;
	header.header["topology"] = "lines";
	const rnd::geometry_files files = rnd::encode_geometry_files(res::tag{ "geomfiles://a/h.geom.desc" }, triangles(1), header);
	BOOST_REQUIRE_MESSAGE(files, files.error);
	BOOST_TEST(!json::parse(text_of(files.desc)).as_object().contains("topology"));
}

// A mesh is drawn by its indices only (geom_manager, glDrawElements): vertices without them are no mesh to write.
BOOST_AUTO_TEST_CASE(VerticesWithoutIndices_AreRefused)
{
	rnd::geometry_desc geometry = triangles(1);
	geometry.indices.clear();
	const std::string error = rnd::write_geometry_files(root(), res::tag{ "geomfiles://a/no_indices.geom.desc" }, geometry);
	BOOST_TEST(mentions(error, "/indices"));
	BOOST_TEST(mentions(error, "no indices"));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "no_indices.geom.desc"));
}

// 32 floats a vertex is the widest "f32xN"; one more makes the vertices bytes.
BOOST_AUTO_TEST_CASE(AVertexOf32Floats_IsF32x32_AndLoadsBack)
{
	const auto layout_of = [](int floats) {
		std::vector<rnd::driver::BufferElement> elements{ { rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" } };
		for (int i = 3; i < floats; ++i) {
			elements.push_back({ rnd::driver::SHADER_DATA_TYPE::FLOAT, std::format("extra{}", i) });
		}
		return rnd::driver::BufferLayout{ elements };
	};
	const auto geometry_of = [&](int floats) {
		rnd::geometry_desc out;
		out.layout = layout_of(floats);
		std::vector<float> values;
		for (int v = 0; v < 3; ++v) {
			for (int f = 0; f < floats; ++f) values.push_back(static_cast<float>(v * floats + f));
		}
		out.vertices.resize(values.size() * sizeof(float));
		std::memcpy(out.vertices.data(), values.data(), out.vertices.size());
		out.indices = { 0, 1, 2 };
		return out;
	};

	const res::tag tag{ "geomfiles://a/wide.geom.desc" };
	const rnd::geometry_desc wide = geometry_of(32);
	BOOST_REQUIRE(rnd::write_geometry_files(root(), tag, wide).empty());
	BOOST_TEST(std::string(desc_on_disk("wide.geom.desc").at("vertices").at("layout").as_string()) == "f32x32");
	const auto loaded = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	BOOST_TEST((loaded->vertices == wide.vertices));

	const rnd::geometry_files wider = rnd::encode_geometry_files(res::tag{ "geomfiles://a/wider.geom.desc" }, geometry_of(33));
	BOOST_REQUIRE_MESSAGE(wider, wider.error);
	BOOST_TEST(std::string(json::parse(text_of(wider.desc)).as_object().at("vertices").at("layout").as_string()) == "u8");
}

// Arrays are checked by the loader too, with what holds for any topology: lines load, an index past the vertices does not.
BOOST_AUTO_TEST_CASE(ArraysThatTheChecksRefuse_DoNotLoad_LinesDo)
{
	const auto arrays = [](std::string_view indices, std::string_view first_bytes, std::string_view topology) {
		std::string text = R"({"__type":"geometry_desc","layout":{"position":"vec3"},)";
		text += topology;
		text += R"("indices":)" + std::string(indices) + R"(,"vertices":[)" + std::string(first_bytes);
		for (int i = 4; i < 36; ++i) text += ",0";
		return text + "]}";
	};
	const auto stored = [&](std::string_view name, const std::string& text) {
		const res::tag tag{ std::string("memory://geometry_files/") + std::string(name) };
		BOOST_REQUIRE(root().store(tag, test_util::to_bytes(text)));
		test_util::pump(tasks);
		return load<rnd::geometry_desc>(tag);
	};

	const auto line = stored("line.geom.desc", arrays("[0,1,1,2]", "0,0,0,0", R"("topology":"lines",)"));
	BOOST_REQUIRE_MESSAGE(line.is_ready() && !line.has_error(), line.error_text());
	BOOST_TEST(line->indices.size() == 4u);

	const auto past = stored("past.geom.desc", arrays("[0,1,3]", "0,0,0,0", ""));
	BOOST_TEST(past.has_error());
	BOOST_TEST(mentions(past.error_text(), "/indices"));

	// 0x7fc00000: a quiet NaN as the x of the first position.
	const auto nan = stored("nan.geom.desc", arrays("[0,1,2]", "0,0,192,127", ""));
	BOOST_TEST(nan.has_error());
	BOOST_TEST(mentions(nan.error_text(), "not finite"));
}

#if SNK_HAS_ASSIMP
// The numbers of #977/#1022 (JSON vertices were ~460 ms of a 1.1 s import in Debug), taken again: logged, never asserted.
// The JSON arrays lines are the form before EPIC-79, measured in the same run on the same data.
BOOST_AUTO_TEST_CASE(GenRobot_ImportAndGeometryLoad_AreTimed)
{
	using clock = std::chrono::steady_clock;
	const auto ms_since = [](clock::time_point start) {
		return std::chrono::duration<double, std::milli>(clock::now() - start).count();
	};
	std::vector<std::byte> model;
	{
		std::ifstream in(fs::path(SNK_TEST_PROJECT_DIR) / "res" / "objects" / "robot" / "gen_robot.glb", std::ios::binary);
		const std::string bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		model.resize(bytes.size());
		std::memcpy(model.data(), bytes.data(), bytes.size());
	}
	BOOST_REQUIRE(!model.empty());

	// assimp runs: nothing remembered from another case.
	scn::remember_model_imports(false);
	struct remember_again
	{
		~remember_again() { scn::remember_model_imports(true); }
	} restore;

	scn::model_importer_adapter importer(storage.require<desc::desc_system>(), root());
	const res::tag geometry_tag{ "memory://objects/robot/gen_robot.geom.desc" };
	clock::time_point start = clock::now();
	const auto prefab = importer(res::tag{ "res://objects/robot/gen_robot.glb" }, model);
	const double import_ms = ms_since(start);
	start = clock::now();
	const auto geometry = load<rnd::geometry_desc>(geometry_tag);
	const double first_load_ms = ms_since(start);
	BOOST_REQUIRE(prefab);
	BOOST_REQUIRE_MESSAGE(geometry.is_ready() && !geometry.has_error(), geometry.error_text());
	BOOST_TEST(root().exists(res::tag{ "memory://objects/robot/gen_robot.geom.bin" }));
	{
		const auto stored = root().fetch_data(geometry_tag);
		BOOST_REQUIRE(stored);
		BOOST_TEST(!rnd::holds_geometry_arrays(json::parse(text_of(stored->get())).as_object()));
	}

	// The same data in both forms, under tags nothing has read yet.
	rnd::geometry_desc data;
	data.layout = geometry->layout;
	data.vertices = geometry->vertices;
	data.indices = geometry->indices;
	data.bounds = geometry->bounds;

	start = clock::now();
	json::object arrays{ { "__type", "geometry_desc" }, { "__parent", "res://base_geometry.desc" } };
	data.serialize(arrays);
	const std::string arrays_text = json::serialize(arrays);
	const double arrays_write_ms = ms_since(start);

	const res::tag binary_tag{ "memory://timing/robot_binary.geom.desc" };
	rnd::geometry_file_options options;
	options.header["__parent"] = "res://base_geometry.desc";
	options.bounds = !data.bounds.empty();
	start = clock::now();
	const rnd::geometry_files files = rnd::encode_geometry_files(binary_tag, data, options);
	const double binary_write_ms = ms_since(start);
	BOOST_REQUIRE_MESSAGE(files, files.error);

	start = clock::now();
	const std::string hash = cmn::sha256_hex(files.binary);
	const double sha_ms = ms_since(start);
	BOOST_TEST(hash.size() == 64u);

	const res::tag arrays_tag{ "memory://timing/robot_arrays.geom.desc" };
	BOOST_REQUIRE(root().store(arrays_tag, test_util::to_bytes(arrays_text)));
	BOOST_REQUIRE(root().store(*res::desc_binary::binary_tag(binary_tag), files.binary));
	BOOST_REQUIRE(root().store(binary_tag, files.desc));
	test_util::pump(tasks);

	start = clock::now();
	const auto from_arrays = load<rnd::geometry_desc>(arrays_tag);
	const double arrays_load_ms = ms_since(start);
	start = clock::now();
	const auto from_binary = load<rnd::geometry_desc>(binary_tag);
	const double binary_load_ms = ms_since(start);
	BOOST_REQUIRE_MESSAGE(from_arrays.is_ready() && !from_arrays.has_error(), from_arrays.error_text());
	BOOST_REQUIRE_MESSAGE(from_binary.is_ready() && !from_binary.has_error(), from_binary.error_text());
	BOOST_TEST((from_arrays->vertices == data.vertices));
	BOOST_TEST((from_binary->vertices == data.vertices));
	BOOST_TEST((from_binary->indices == data.indices));
	BOOST_TEST(from_binary->bounds.size() == from_arrays->bounds.size());

	egLOG_INFO("test/timing", "gen_robot: import (assimp, memory:// files): {:.1f} ms; first geometry load: {:.1f} ms", import_ms, first_load_ms);
	egLOG_INFO("test/timing", "gen_robot: geometry write, JSON arrays: {:.1f} ms ({} bytes); binary pair: {:.1f} ms ({} + {} bytes)",
		arrays_write_ms, arrays_text.size(), binary_write_ms, files.desc.size(), files.binary.size());
	egLOG_INFO("test/timing", "gen_robot: geometry load, JSON arrays: {:.1f} ms; binary file (verify_hash_on_load): {:.1f} ms; "
		"SHA-256 of the binary file: {:.1f} ms", arrays_load_ms, binary_load_ms, sha_ms);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
