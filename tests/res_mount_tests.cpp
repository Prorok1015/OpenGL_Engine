#include <boost/test/unit_test.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/resource/res_resource_service_init.h>
#include <snk/resource/res_shipping_dirs.h>
#include <snk/config/cfg_variables.h>
#include <snk/platform/plt_process.h>
#include <snk/project/prj_project.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/common/logger/engine_log.h>
// Not test_scene_env.hpp: res_vfs_resolver.h brings in Asio and so <windows.h>, whose OPAQUE/TRANSPARENT macros
// break the render headers that one includes.
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <system_error>
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

std::string load(const res::vfs_resolver& r, std::string_view tag)
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

BOOST_FIXTURE_TEST_CASE(ProjectMountTable_EngineUnderTheContentRootsFirstOnTop, layers_fixture)
{
	const fs::path shared = dir.path() / "project" / "shared";
	const fs::path cache = dir.path() / "project" / ".cache";
	const fs::path memory = cache / "memory";
	const fs::path user = cache / "user_profile";

	const auto table = res::resource_system::project_mount_table(engine, { project, shared }, cache, user);

	BOOST_REQUIRE(table.size() == 5u);
	BOOST_TEST(table[0].protocol == res::tag::default_protocol());
	BOOST_TEST((table[0].roots == std::vector<fs::path>{ engine, shared, project }));
	BOOST_TEST((table[0].access == res::mount_access::read_write));
	BOOST_TEST(table[1].protocol == res::tag::memory);
	BOOST_TEST((table[1].roots == std::vector<fs::path>{ memory }));
	BOOST_TEST(table[2].protocol == res::tag::cache);
	BOOST_TEST((table[2].roots == std::vector<fs::path>{ cache }));
	BOOST_TEST(table[3].protocol == res::tag::user);
	BOOST_TEST((table[3].roots == std::vector<fs::path>{ user }));
	BOOST_TEST(table[4].protocol == res::tag::engine);
	BOOST_TEST((table[4].roots == std::vector<fs::path>{ engine }));
	BOOST_TEST((table[4].access == res::mount_access::read_only));
	// Only a table: the caller creates the folders.
	BOOST_TEST(!fs::exists(memory));
}

BOOST_FIXTURE_TEST_CASE(ProjectMountTable_WithoutEngineBaseHasNoEngineLayer, layers_fixture)
{
	const fs::path cache = dir.path() / "project" / ".cache";
	const auto table = res::resource_system::project_mount_table({}, { project }, cache, cache / "user_profile");

	BOOST_REQUIRE(table.size() == 4u);
	BOOST_TEST((table[0].roots == std::vector<fs::path>{ project }));
	for (const auto& mount : table) {
		BOOST_TEST(mount.protocol != res::tag::engine);
	}
}

BOOST_FIXTURE_TEST_CASE(ProjectMountTable_ResReportsEveryFile_OutsideGit, layers_fixture)
{
	const fs::path cache = dir.path() / "project" / ".cache";
	const auto table = res::resource_system::project_mount_table({}, { project }, cache, cache / "user_profile");

	BOOST_REQUIRE(!table.empty());
	BOOST_TEST(table[0].protocol == res::tag::default_protocol());
	const res::watch_options& options = table[0].watch;
	BOOST_TEST(options.report_changes);
	BOOST_REQUIRE(static_cast<bool>(options.ignore_dir));
	BOOST_TEST(options.ignore_dir(".git"));
	BOOST_TEST(!options.ignore_dir(".shared"));
	BOOST_TEST(!options.ignore_dir("textures"));
	BOOST_TEST(!options.ignore_dir(".git2"));
	BOOST_TEST(!static_cast<bool>(options.accept_file));
	// Storing works as before; removing and renaming stay code:// only.
	BOOST_TEST(!table[0].file_operations);
}

BOOST_FIXTURE_TEST_CASE(CacheAndUser_ResolveAndStoreInTheirDirectories, layers_fixture)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	const fs::path cache = dir.path() / "project" / ".cache";
	const fs::path user = cache / "user_profile";
	fs::create_directories(cache / "memory");
	fs::create_directories(user);
	res::resource_system sys(tasks, res::resource_system::project_mount_table({}, { project }, cache, user));

	BOOST_TEST(sys.store(res::tag{ "cache://thumbs/a.png" }, to_bytes("thumb")));
	BOOST_TEST(sys.store(res::tag{ "user://saves/slot1.json" }, to_bytes("save")));
	BOOST_TEST(wait_for_content(cache / "thumbs" / "a.png", "thumb"));
	BOOST_TEST(wait_for_content(user / "saves" / "slot1.json", "save"));
	BOOST_TEST(sys.exists(res::tag{ "cache://thumbs/a.png" }));
	BOOST_TEST(sys.exists(res::tag{ "user://saves/slot1.json" }));

	// Neither leaks into the content: the project's res:// does not see them.
	BOOST_TEST(!sys.exists(res::tag{ "res://thumbs/a.png" }));
	BOOST_TEST(!sys.exists(res::tag{ "res://saves/slot1.json" }));
	BOOST_TEST(!sys.store(res::tag{ "user://../../escape.txt" }, to_bytes("x")));
}

BOOST_FIXTURE_TEST_CASE(ShippingMountTable_ContentAndEngineReadOnly_OnlyUserAndMemoryWritable, layers_fixture)
{
	const fs::path shared = dir.path() / "project" / "shared";
	const fs::path user = dir.path() / "player";
	const fs::path scratch = dir.path() / "scratch";

	const auto table = res::resource_system::shipping_mount_table(engine, { project, shared }, user, scratch);

	BOOST_REQUIRE(table.size() == 4u);
	BOOST_TEST(table[0].protocol == res::tag::default_protocol());
	BOOST_TEST((table[0].roots == std::vector<fs::path>{ engine, shared, project }));
	BOOST_TEST((table[0].access == res::mount_access::read_only));
	BOOST_TEST(!table[0].watch.report_changes);
	BOOST_TEST(table[1].protocol == res::tag::memory);
	BOOST_TEST((table[1].roots == std::vector<fs::path>{ scratch }));
	BOOST_TEST((table[1].access == res::mount_access::read_write));
	BOOST_TEST(table[2].protocol == res::tag::user);
	BOOST_TEST((table[2].roots == std::vector<fs::path>{ user }));
	BOOST_TEST((table[2].access == res::mount_access::read_write));
	BOOST_TEST(table[3].protocol == res::tag::engine);
	BOOST_TEST((table[3].access == res::mount_access::read_only));
	for (const auto& mount : table) {
		BOOST_TEST(mount.protocol != res::tag::cache);
		BOOST_TEST(mount.protocol != res::tag::code);
		BOOST_TEST(!mount.file_operations);
	}
	BOOST_TEST(!fs::exists(user));
	BOOST_TEST(!fs::exists(scratch));
}

BOOST_FIXTURE_TEST_CASE(ShippingMounts_RefuseWritesToTheProjectAndTheEngine_AcceptUserAndMemory, layers_fixture)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	const fs::path user = dir.path() / "player";
	const fs::path scratch = dir.path() / "scratch";
	fs::create_directories(user);
	fs::create_directories(scratch);
	res::resource_system sys(tasks, res::resource_system::shipping_mount_table(engine, { project }, user, scratch));

	BOOST_TEST(sys.exists(res::tag{ "res://levels/main.desc" }));
	BOOST_TEST(sys.exists(res::tag{ "res://shaders/only_engine.frag" }));
	BOOST_TEST(!sys.store_sync(res::tag{ "res://levels/main.desc" }, to_bytes("x")));
	BOOST_TEST(!sys.store_sync(res::tag{ "res://levels/new.desc" }, to_bytes("x")));
	BOOST_TEST(!sys.create_sync(res::tag{ "res://levels/created.desc" }, to_bytes("x")));
	BOOST_TEST(!sys.store_sync(res::tag{ "engine://shaders/pbr.frag" }, to_bytes("x")));
	BOOST_TEST(read_file(project / "levels" / "main.desc") == "project-level");
	BOOST_TEST(!fs::exists(project / "levels" / "new.desc"));
	BOOST_TEST(!fs::exists(project / "levels" / "created.desc"));
	BOOST_TEST(read_file(engine / "shaders" / "pbr.frag") == "engine-pbr");

	BOOST_TEST(sys.store_sync(res::tag{ "user://saves/slot1.json" }, to_bytes("save")));
	BOOST_TEST(sys.store_sync(res::tag{ "memory://objects/a.desc" }, to_bytes("scratch")));
	BOOST_TEST(read_file(user / "saves" / "slot1.json") == "save");
	BOOST_TEST(read_file(scratch / "objects" / "a.desc") == "scratch");
	BOOST_TEST(!sys.exists(res::tag{ "res://saves/slot1.json" }));
}

BOOST_FIXTURE_TEST_CASE(NotMountedProtocol_TheErrorNamesTheMountedOnes, layers_fixture)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	const fs::path user = dir.path() / "player";
	const fs::path scratch = dir.path() / "scratch";
	fs::create_directories(user);
	fs::create_directories(scratch);
	res::resource_system sys(tasks, res::resource_system::shipping_mount_table(engine, { project }, user, scratch));

	const std::string expected = "'cache://' is not mounted in this run (mounted: engine, memory, res, user)";
	BOOST_TEST(sys.not_mounted_error("cache") == expected);
	BOOST_TEST(!sys.exists(res::tag{ "cache://thumbs/a.png" }));
	const res::file_operation created = sys.create_sync(res::tag{ "cache://thumbs/a.png" }, to_bytes("x"));
	BOOST_TEST(!created);
	BOOST_TEST(created.error == expected);
	BOOST_TEST(sys.remove(res::tag{ "cache://thumbs/a.png" }).error == expected);
}

// #631: whatever a tag without a protocol begins with, it reaches no resolver, and the error says how to write it.
BOOST_FIXTURE_TEST_CASE(ATagWithoutAProtocol_ReachesNoMount_AndTheErrorSaysToWriteRes, layers_fixture)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	const fs::path user = dir.path() / "player";
	const fs::path scratch = dir.path() / "scratch";
	dir.write("player/xa.png", "secret");
	dir.write("player/a.png", "secret");
	fs::create_directories(scratch);
	res::resource_system sys(tasks, res::resource_system::shipping_mount_table(engine, { project }, user, scratch));

	BOOST_TEST(sys.not_mounted_error(res::tag{ "materials/a.mat.desc" })
		== "'materials/a.mat.desc' has no protocol: write res://materials/a.mat.desc");
	for (const char* text : { "user/xa.png", "user:/xa.png", "user\\xa.png", "a:b://xa.png", "res://user://a.png", "shaders/pbr.frag" }) {
		BOOST_TEST_CONTEXT(text) {
			const res::tag tag{ text };
			BOOST_TEST(tag.protocol().empty());
			BOOST_TEST(!sys.exists(tag));
			BOOST_TEST(!sys.fetch_data(tag));
			const res::file_operation created = sys.create_sync(tag, to_bytes("x"));
			BOOST_TEST(!created);
			BOOST_TEST(created.error.find("has no protocol: write res://") != std::string::npos);
		}
	}
	BOOST_TEST(read_file(user / "a.png") == "secret");
	BOOST_TEST(read_file(user / "xa.png") == "secret");

	res::vfs_resolver layered("res", { engine, project });
	BOOST_TEST(load(layered, "res:///etc/passwd").rfind("ERR:", 0) == 0u);
	BOOST_TEST(load(layered, "res://shaders\\..\\..\\..\\engine\\shaders\\pbr.frag").rfind("ERR:", 0) == 0u);
	BOOST_TEST(load(layered, "res://a\\..\\..\\x").rfind("ERR:", 0) == 0u);
	using namespace std::string_view_literals;
	BOOST_TEST(load(layered, "res://shaders/pbr.frag\0.txt"sv).rfind("ERR:", 0) == 0u);
	BOOST_TEST(!sys.exists(res::tag{ "res://levels/main.desc\0x"sv }));
}

BOOST_FIXTURE_TEST_CASE(Store_OverAnEngineOnlyFileIsLogged, layers_fixture)
{
	std::mutex m;
	std::vector<std::string> warnings;
	auto sink = engine::add_log_sink([&](std::string_view, engine::log_level level, std::string_view message) {
		if (level != engine::log_level::warn) return;
		std::lock_guard lock(m);
		warnings.emplace_back(message);
	});
	const auto shadow_warnings = [&] {
		std::lock_guard lock(m);
		return std::count_if(warnings.begin(), warnings.end(),
			[](const std::string& w) { return w.find("overrides the file of a lower layer") != std::string::npos; });
	};

	res::vfs_resolver r("res", { engine, project });
	BOOST_TEST(r.store(res::tag{ "res://shaders/only_engine.frag" }, to_bytes("override")));
	BOOST_TEST(shadow_warnings() == 1);
	BOOST_TEST(wait_for_content(project / "shaders" / "only_engine.frag", "override"));

	// The project has its own copy now, and a file the engine lacks is no override: neither is logged.
	BOOST_TEST(r.store(res::tag{ "res://shaders/only_engine.frag" }, to_bytes("again")));
	BOOST_TEST(r.store(res::tag{ "res://levels/new.desc" }, to_bytes("new")));
	BOOST_TEST(shadow_warnings() == 1);
}

BOOST_FIXTURE_TEST_CASE(ReadOnlyMount_RefusesErase, layers_fixture)
{
	res::vfs_resolver r("engine", { engine }, res::mount_access::read_only);
	BOOST_TEST(!r.erase(res::tag{ "engine://shaders/pbr.frag" }));
	BOOST_TEST(fs::exists(engine / "shaders" / "pbr.frag"));
}

BOOST_FIXTURE_TEST_CASE(PinnedRoot_ARootSwappedForASymlinkDoesNotMoveTheMount, layers_fixture)
{
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	res::vfs_resolver r("res", { project });

	const fs::path moved = dir.path() / "project" / "res_moved";
	fs::rename(project, moved);
	std::error_code ec;
	fs::create_directory_symlink(outside, project, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	BOOST_TEST(!r.store(res::tag{ "res://planted.txt" }, to_bytes("x")));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	BOOST_TEST(!fs::exists(outside / "planted.txt"));
}

BOOST_FIXTURE_TEST_CASE(ResourceInit_MountsAValidProjectAndCreatesItsWritableFolders, layers_fixture)
{
	dir.write("project/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	auto opened = prj::open_project(dir.path() / "project");
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);

	ds::app_data_storage data;
	data.construct<ds::task_system>(test_util::manual_tasks());
	data.construct<prj::project_context>(*opened.context);
	core::res::resource_init(data);

	const auto& project_ctx = data.require<prj::project_context>();
	BOOST_TEST(fs::is_directory(project_ctx.memory_dir()));
	BOOST_TEST(fs::is_directory(project_ctx.user_profile_dir()));
	BOOST_TEST(!fs::exists(project_ctx.build_dir())); // not a mount: nothing creates it
	BOOST_TEST(data.require<res::resource_system>().get_resources_path() == fs::canonical(project));
	BOOST_TEST(data.require<res::resource_system>().exists(res::tag{ "res://levels/main.desc" }));
	core::res::resource_term(data);
}

BOOST_FIXTURE_TEST_CASE(ResourceInit_RefusesASymlinkedCacheWithAProjectError, layers_fixture)
{
	dir.write("project/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	auto opened = prj::open_project(dir.path() / "project");
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);

	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	std::error_code ec;
	fs::create_directory_symlink(outside, opened.context->cache_dir(), ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	ds::app_data_storage data;
	data.construct<ds::task_system>(test_util::manual_tasks());
	data.construct<prj::project_context>(*opened.context);
	BOOST_CHECK_THROW(core::res::resource_init(data), prj::project_error);
	BOOST_TEST(!data.has_value<res::resource_system>());
	BOOST_TEST(!fs::exists(outside / "memory"));
}

namespace {
// res/read_only_project and resource/user_path for one test, as the game host sets them.
struct read_only_project_config
{
	explicit read_only_project_config(const fs::path& user)
	{
		cfg::set_variable("res/read_only_project", "true");
		cfg::set_variable("resource/user_path", user.string());
	}
	~read_only_project_config()
	{
		cfg::set_variable("res/read_only_project", "false");
		cfg::set_variable("resource/user_path", "");
	}
	read_only_project_config(const read_only_project_config&) = delete;
	read_only_project_config& operator=(const read_only_project_config&) = delete;
};
}

BOOST_FIXTURE_TEST_CASE(ResourceInit_ReadOnlyProject_WritesNothingIntoTheProject, layers_fixture)
{
	dir.write("project/project.snkproj", R"json({ "format_version": 1, "name": "Read Only", "engine_version": "0.2.0" })json");
	auto opened = prj::open_project(dir.path() / "project");
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	const fs::path user = dir.path() / "player";
	read_only_project_config config{ user };

	ds::app_data_storage data;
	data.construct<ds::task_system>(test_util::manual_tasks());
	data.construct<prj::project_context>(*opened.context);
	core::res::resource_init(data);

	const auto& project_ctx = data.require<prj::project_context>();
	auto& sys = data.require<res::resource_system>();
	BOOST_TEST(!fs::exists(project_ctx.cache_dir()));
	BOOST_TEST((sys.mount_roots(res::tag::user) == std::vector<fs::path>{ user }));
	const auto scratch = sys.mount_roots(res::tag::memory);
	BOOST_REQUIRE(scratch.size() == 1u);
	const std::string own_prefix = "snk-Read Only-" + std::to_string(platform::current_process_id()) + "-";
	BOOST_TEST(scratch.front().filename().string().starts_with(own_prefix));
	BOOST_TEST(fs::is_directory(scratch.front()));
#ifndef _WIN32
	BOOST_TEST(((fs::status(scratch.front()).permissions() & fs::perms::all) == fs::perms::owner_all));
#endif
	BOOST_TEST(sys.mount_roots(res::tag::cache).empty());

	BOOST_TEST(sys.exists(res::tag{ "res://levels/main.desc" }));
	BOOST_TEST(!sys.store_sync(res::tag{ "res://levels/main.desc" }, to_bytes("x")));
	BOOST_TEST(read_file(project / "levels" / "main.desc") == "project-level");
	BOOST_TEST(sys.store_sync(res::tag{ "user://saves/slot1.json" }, to_bytes("save")));
	BOOST_TEST(read_file(user / "saves" / "slot1.json") == "save");

	core::res::resource_term(data);
	BOOST_TEST(!fs::exists(scratch.front()));
	BOOST_TEST(fs::is_directory(user));
}

BOOST_AUTO_TEST_CASE(SafeFolderName_KeepsPlainNamesAndLetters_ReplacesTheRest)
{
	BOOST_TEST(core::res::safe_folder_name("My Game_2.0-beta") == "My Game_2.0-beta");
	BOOST_TEST(core::res::safe_folder_name("a/b\\c:d*e") == "a_b_c_d_e");
	BOOST_TEST(core::res::safe_folder_name("Game. ") == "Game");
	BOOST_TEST(core::res::safe_folder_name("") == "game");
	BOOST_TEST(core::res::safe_folder_name("..") == "game");
	// Letters outside ASCII stay: two such names are two folders.
	BOOST_TEST(core::res::safe_folder_name("\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0") == "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0");
	BOOST_TEST(core::res::safe_folder_name("\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0") != core::res::safe_folder_name("\xD0\x9C\xD0\xB0\xD0\xBC\xD0\xB0"));
}

BOOST_AUTO_TEST_CASE(SafeFolderName_InvalidUtf8BecomesUnderscores)
{
	BOOST_TEST(core::res::safe_folder_name("Caf\xE9") == "Caf_");          // Latin-1, not UTF-8
	BOOST_TEST(core::res::safe_folder_name("a\xC0\xAF" "b") == "a__b");     // overlong '/'
	BOOST_TEST(core::res::safe_folder_name("\xED\xA0\x80x") == "___x");    // a surrogate
	BOOST_TEST(core::res::safe_folder_name("ab\xD0") == "ab_");             // cut in the middle of a letter
	BOOST_TEST(core::res::safe_folder_name("\xF4\x90\x80\x80") == "____"); // above U+10FFFF
	BOOST_TEST(core::res::safe_folder_name("\xF0\x9F\x8E\xAE") == "\xF0\x9F\x8E\xAE"); // a valid 4-byte character
}

BOOST_AUTO_TEST_CASE(SafeFolderName_ReservedWindowsNamesAreChanged)
{
	BOOST_TEST(core::res::safe_folder_name("COM\xC2\xB9") == "COM\xC2\xB9_");
	BOOST_TEST(core::res::safe_folder_name("lpt\xC2\xB3.txt") == "lpt\xC2\xB3.txt_");
	BOOST_TEST(core::res::safe_folder_name("CON") == "CON_");
	BOOST_TEST(core::res::safe_folder_name("nul") == "nul_");
	BOOST_TEST(core::res::safe_folder_name("Aux.txt") == "Aux.txt_");
	BOOST_TEST(core::res::safe_folder_name("COM1") == "COM1_");
	BOOST_TEST(core::res::safe_folder_name("lpt9.save") == "lpt9.save_");
	BOOST_TEST(core::res::safe_folder_name("COM0") == "COM0");
	BOOST_TEST(core::res::safe_folder_name("CONSOLE") == "CONSOLE");
}

BOOST_AUTO_TEST_CASE(SafeFolderName_IsAtMost64Bytes_CutOnACharacter)
{
	BOOST_TEST(core::res::safe_folder_name(std::string(100, 'a')) == std::string(64, 'a'));
	// 63 ASCII bytes, then a two-byte letter that would straddle byte 64: it goes whole.
	const std::string straddling = std::string(63, 'a') + "\xD0\x98" + "bc";
	BOOST_TEST(core::res::safe_folder_name(straddling) == std::string(63, 'a'));
	BOOST_TEST(core::res::safe_folder_name(std::string(63, 'a') + " x") == std::string(63, 'a'));
}

namespace {
// A scratch folder of `name` for process `pid`, as make_scratch_dir names it, private to this user.
fs::path planted_scratch(const std::string& name, std::int64_t pid)
{
	std::string error;
	const fs::path base = core::res::scratch_base_dir(error);
	BOOST_REQUIRE_MESSAGE(!base.empty(), error);
	const fs::path folder = base / ("snk-" + name + "-" + std::to_string(pid) + "-0123456789abcdef");
	fs::create_directories(folder / "objects");
	fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace);
	return folder;
}
}

BOOST_AUTO_TEST_CASE(MakeScratchDir_MakesANewPrivateFolderEachTime)
{
	const auto first = core::res::make_scratch_dir("res_mount_tests_make");
	const auto second = core::res::make_scratch_dir("res_mount_tests_make");
	BOOST_REQUIRE_MESSAGE(!first.path.empty(), first.error);
	BOOST_REQUIRE_MESSAGE(!second.path.empty(), second.error);
	BOOST_TEST(first.path != second.path);
	BOOST_TEST(fs::is_directory(first.path));
#ifndef _WIN32
	BOOST_TEST(((fs::status(first.path).permissions() & fs::perms::all) == fs::perms::owner_all));
#endif
	std::error_code ec;
	fs::remove_all(first.path, ec);
	fs::remove_all(second.path, ec);
}

BOOST_AUTO_TEST_CASE(RemoveStaleScratchDirs_RemovesOnlyThePrivateFoldersOfEndedRuns)
{
	const std::string name = "res_mount_tests_stale";
	// No process has this id on Linux (pid_max <= 4194304) or Windows (ids are multiples of 4).
	constexpr std::int64_t ended_pid = 2147483647;
	const fs::path ended = planted_scratch(name, ended_pid);
	const fs::path own = planted_scratch(name, platform::current_process_id());
	const fs::path other_game = planted_scratch("res_mount_tests_other", ended_pid);
	std::string error;
	const fs::path base = core::res::scratch_base_dir(error);
	const fs::path not_a_scratch_name = base / ("snk-" + name + "-" + std::to_string(ended_pid));
	fs::create_directories(not_a_scratch_name);
#ifndef _WIN32
	// Readable by others: not one this game made, so it is not walked.
	const fs::path open_folder = base / ("snk-" + name + "-" + std::to_string(ended_pid) + "-fedcba9876543210");
	fs::create_directories(open_folder);
	fs::permissions(open_folder, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
	// A link to a folder of the player: neither it nor what it points at goes.
	test_util::temp_dir victim{ "res_mount_tests_victim" };
	victim.write("keep.txt", "player data");
	const fs::path link = base / ("snk-" + name + "-" + std::to_string(ended_pid) + "-00000000000000aa");
	std::error_code link_ec;
	fs::create_directory_symlink(victim.path(), link, link_ec);
#endif

	BOOST_TEST(core::res::remove_stale_scratch_dirs(name) == 1u);
	BOOST_TEST(!fs::exists(ended));
	BOOST_TEST(fs::is_directory(own));
	BOOST_TEST(fs::is_directory(other_game));
	BOOST_TEST(fs::is_directory(not_a_scratch_name));
#ifndef _WIN32
	BOOST_TEST(fs::is_directory(open_folder));
	if (!link_ec) {
		BOOST_TEST(fs::is_symlink(link));
		BOOST_TEST(read_file(victim.path() / "keep.txt") == "player data");
	}
#endif

	std::error_code ec;
	for (const auto& folder : { own, other_game, not_a_scratch_name }) {
		fs::remove_all(folder, ec);
	}
#ifndef _WIN32
	fs::remove_all(open_folder, ec);
	fs::remove(link, ec);
#endif
}

BOOST_AUTO_TEST_SUITE_END()
