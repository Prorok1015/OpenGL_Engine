#include <boost/test/unit_test.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
// Not test_scene_env.hpp: res_vfs_resolver.h brings in Asio and so <windows.h>, whose OPAQUE/TRANSPARENT macros
// break the render headers that one includes.
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Layered res:// (project over engine), read-only engine:// and the mount tables of EPIC-42 (#154).
namespace {

namespace fs = std::filesystem;

std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> out(s.size());
	for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
	return out;
}

std::string load(const res::vfs_resolver& r, const char* tag)
{
	auto block = r.resolve(res::tag{ tag });
	block->wait();
	if (block->has_error()) return "ERR:" + block->error_msg;
	std::string s(block->data.size(), '\0');
	for (std::size_t i = 0; i < block->data.size(); ++i) s[i] = static_cast<char>(block->data[i]);
	return s;
}

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

bool wait_for_content(const fs::path& p, std::string_view expected)
{
	for (int i = 0; i < 500; ++i) {
		if (fs::exists(p) && read_file(p) == expected) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

struct layers_fixture
{
	test_util::temp_dir dir{ "res_mounts" };
	fs::path engine = dir.path() / "engine";
	fs::path project = dir.path() / "project" / "res";

	layers_fixture()
	{
		dir.write("engine/shaders/pbr.frag", "engine-pbr");
		dir.write("engine/shaders/only_engine.frag", "engine-only");
		dir.write("project/res/shaders/pbr.frag", "project-pbr");
		dir.write("project/res/levels/main.desc", "project-level");
	}
};

}

BOOST_AUTO_TEST_SUITE(ResMountTests)

BOOST_FIXTURE_TEST_CASE(ProjectLayer_OverridesEngineAndFallsBack, layers_fixture)
{
	res::vfs_resolver r("res", { engine, project });
	BOOST_TEST(load(r, "res://shaders/pbr.frag") == "project-pbr");
	BOOST_TEST(load(r, "res://shaders/only_engine.frag") == "engine-only");
	BOOST_TEST(load(r, "res://levels/main.desc") == "project-level");
}

BOOST_FIXTURE_TEST_CASE(Store_WritesToTheProjectEvenForAnEngineOnlyFile, layers_fixture)
{
	res::vfs_resolver r("res", { engine, project });
	BOOST_TEST(r.store(res::tag{ "res://shaders/only_engine.frag" }, to_bytes("override")));

	BOOST_TEST(wait_for_content(project / "shaders" / "only_engine.frag", "override"));
	BOOST_TEST(read_file(engine / "shaders" / "only_engine.frag") == "engine-only");
	BOOST_TEST(load(r, "res://shaders/only_engine.frag") == "override");
}

BOOST_FIXTURE_TEST_CASE(ReadOnlyMount_RefusesStoreAndKeepsServing, layers_fixture)
{
	res::vfs_resolver r("engine", { engine }, res::mount_access::read_only);
	BOOST_TEST(!r.store(res::tag{ "engine://shaders/pbr.frag" }, to_bytes("x")));
	BOOST_TEST(!r.store(res::tag{ "engine://shaders/new.frag" }, to_bytes("x")));

	BOOST_TEST(load(r, "engine://shaders/pbr.frag") == "engine-pbr");
	BOOST_TEST(read_file(engine / "shaders" / "pbr.frag") == "engine-pbr");
	BOOST_TEST(!fs::exists(engine / "shaders" / "new.frag"));
	BOOST_TEST(!r.exists(res::tag{ "engine://shaders/new.frag" }));
}

BOOST_FIXTURE_TEST_CASE(EveryMount_RejectsTagsLeavingItsRoots, layers_fixture)
{
	res::vfs_resolver layered("res", { engine, project });
	res::vfs_resolver engine_only("engine", { engine }, res::mount_access::read_only);
	BOOST_TEST(load(layered, "res://../engine/shaders/pbr.frag").rfind("ERR:", 0) == 0u);
	BOOST_TEST(load(engine_only, "engine://../project/res/levels/main.desc").rfind("ERR:", 0) == 0u);
	BOOST_TEST(!layered.store(res::tag{ "res://../escape.txt" }, to_bytes("x")));
	BOOST_TEST(!fs::exists(dir.path() / "project" / "escape.txt"));
}

BOOST_FIXTURE_TEST_CASE(ResourceSystem_ExposesLayersOfTheMountTable, layers_fixture)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	const fs::path memory = dir.path() / "project" / ".cache" / "memory";
	fs::create_directories(memory);

	res::resource_system sys(tasks, res::mount_table{
		res::mount{ res::tag::default_protocol(), { engine, project }, res::mount_access::read_write },
		res::mount{ res::tag::memory, { memory }, res::mount_access::read_write },
		res::mount{ res::tag::engine, { engine }, res::mount_access::read_only },
	});

	BOOST_REQUIRE(sys.get_resource_layers().size() == 2u);
	BOOST_TEST(sys.get_resource_layers().front() == engine);
	BOOST_TEST(sys.get_resources_path() == project);

	BOOST_TEST(sys.exists(res::tag{ "res://shaders/only_engine.frag" }));
	BOOST_TEST(sys.exists(res::tag{ "res://levels/main.desc" }));
	BOOST_TEST(sys.exists(res::tag{ "engine://shaders/pbr.frag" }));
	BOOST_TEST(!sys.exists(res::tag{ "engine://levels/main.desc" }));
	BOOST_TEST(!sys.store(res::tag{ "engine://shaders/pbr.frag" }, to_bytes("x")));
}

BOOST_FIXTURE_TEST_CASE(ProjectMountTable_PutsTheFirstContentRootOnTop, layers_fixture)
{
	// resource/base_path is empty in the unit tests, so there is no engine layer and no engine:// here.
	const fs::path shared = dir.path() / "project" / "shared";
	fs::create_directories(shared);
	const fs::path memory = dir.path() / "project" / ".cache" / "memory";

	const auto table = res::resource_system::project_mount_table({ project, shared }, memory);

	BOOST_TEST(fs::is_directory(memory));
	BOOST_REQUIRE(table.size() == 2u);
	BOOST_TEST(table[0].protocol == res::tag::default_protocol());
	BOOST_REQUIRE(table[0].roots.size() == 2u);
	BOOST_TEST(table[0].roots[0] == shared);
	BOOST_TEST(table[0].roots[1] == project);
	BOOST_TEST((table[0].access == res::mount_access::read_write));
	BOOST_TEST(table[1].protocol == res::tag::memory);
	BOOST_REQUIRE(table[1].roots.size() == 1u);
	BOOST_TEST(table[1].roots[0] == memory);
}

BOOST_AUTO_TEST_SUITE_END()
