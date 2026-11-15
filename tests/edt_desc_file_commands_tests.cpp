#include <boost/test/unit_test.hpp>
#include "test_asset_ids_index.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include "edt_tag_text.h"
#include <snk/command/cmd_policy.h>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_describe_ops.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/desc/desc_system.h>
#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// desc.read / desc.create / desc.write (#550): the paths, the schema check before a byte is written, the version
// rechecked at the write, and the files that are not the project's own.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// A map of tags: each value is read as a reference, so each one follows the rule of #697.
class texture_table_desc : public desc::schema_desc<texture_table_desc>
{
public:
	static constexpr std::string_view __type = "test.texture_table";

	std::map<std::string, res::tag> textures;

	BOOST_DESCRIBE_CLASS(texture_table_desc, (), (textures), (), ())
};

const char* const k_material = R"({"__type":"material_desc","queue":"mix","defines":["A"]})";
const char* const k_index_wait = "the asset index is still being built, so a guid cannot be checked; call again when it is ready";

// The refusal an agent gets while the open level has a worlds entry the path rules reject (#966).
bool says_which_entry(std::string_view text)
{
	return text.find("the open level 'levels/open.desc' has a worlds entry '") != std::string_view::npos
		&& text.find("that the path rules reject (") != std::string_view::npos
		&& text.find("desc files are not written until it is fixed or the level is closed") != std::string_view::npos;
}

// engine:// served from `root` for one test; its resolver goes before the folder does.
struct engine_mount_scope
{
	engine_mount_scope(res::resource_system& resources, const fs::path& root) : m_resources(resources)
	{
		if (!m_resources.mount_roots(res::tag::engine).empty()) {
			m_resources.remount(res::tag::engine, { root });
			return;
		}
		res::mount added;
		added.protocol = res::tag::engine;
		added.roots = { root };
		added.access = res::mount_access::read_only;
		m_resources.add_mount(std::move(added));
	}
	~engine_mount_scope() { m_resources.unregistrate_resolver(res::tag::engine); }
	engine_mount_scope(const engine_mount_scope&) = delete;
	engine_mount_scope& operator=(const engine_mount_scope&) = delete;

private:
	res::resource_system& m_resources;
};

struct desc_fixture : test_util::scene_env
{
	desc_fixture()
	{
		fs::create_directories(res_root / "materials");
		fs::create_directories(lower / "materials");
		rs->unregistrate_resolver(res::tag::default_protocol());
		// The lower layer first, the project's own top layer last (it receives the writes), as the editor mounts them.
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		seen = events.agent_action.subscribe([this](const edt::agent_action_info& info) { actions.push_back(info); });
		descs->register_desc<texture_table_desc>("test.texture_table");
		edt::desc_file_deps deps{ *rs, *descs };
		deps.top_layer = [this] { return res_root; };
		deps.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		deps.open_level_files = [this] { return open_level; };
		deps.events = &events;
		deps.write_blocker = [this] { return blocked; };
		deps.asset_index_wait = [this] { return index_pending ? std::optional<std::string>(k_index_wait) : std::nullopt; };
		registrations = edt::register_desc_file_commands(commands, std::move(deps));
	}

	~desc_fixture()
	{
		registrations.clear();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	cmd::result call(const std::string& id, json::value args, cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, std::move(args), std::move(from));
	}

	static cmd::call call_of(json::object args)
	{
		cmd::call out;
		out.args = std::move(args);
		out.from = cmd::origin::agent("s1");
		return out;
	}

	json::object desc_args(const std::string& path, const char* content) const
	{
		return json::object{ { "path", path }, { "desc", json::parse(content) } };
	}

	json::object write_args(const std::string& path, const std::string& version, const char* content) const
	{
		return json::object{ { "path", path }, { "version", version }, { "desc", json::parse(content) } };
	}

	static std::string text_of(const json::value& value)
	{
		return std::string(value.as_string());
	}

	std::string version_of(const std::string& path)
	{
		const auto read = call("desc.read", json::object{ { "path", path } });
		BOOST_REQUIRE_MESSAGE(read.ok, read.error);
		return std::string(read.value.at("version").as_string());
	}

	void make_material(const std::string& path, const char* content = k_material)
	{
		const auto made = call("desc.create", desc_args(path, content));
		BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	}

	test_util::temp_dir dir{ "edt_desc_file_commands" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	edt::editor_events events;
	std::vector<edt::agent_action_info> actions;
	edt::connection seen;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
	std::vector<std::string> open_level;
	std::optional<std::string> blocked;
	bool index_pending = false;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDescFileCommands, desc_fixture)

BOOST_AUTO_TEST_CASE(TheCommands_HaveTheirCategoriesAndOnlyAWriteAsksAPerson)
{
	BOOST_TEST((commands.find("desc.read")->category == cmd::agent_category::read));
	BOOST_TEST((commands.find("desc.create")->category == cmd::agent_category::scene_edit));
	BOOST_TEST((commands.find("desc.write")->category == cmd::agent_category::scene_edit));
	BOOST_TEST((commands.find("desc.read")->risk == cmd::danger::none));
	BOOST_TEST((commands.find("desc.create")->risk == cmd::danger::none));
	BOOST_TEST((commands.find("desc.write")->risk == cmd::danger::confirm));
	BOOST_TEST(static_cast<bool>(commands.find("desc.write")->describe));
}

BOOST_AUTO_TEST_CASE(AnAgentCreatesAMaterialAndChangesIt)
{
	const auto made = call("desc.create", desc_args("materials/red.mat.desc", k_material));
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	BOOST_TEST(fs::exists(res_root / "materials" / "red.mat.desc"));
	BOOST_TEST(read_file(res_root / "materials" / "red.mat.desc").find("\"__version\"") != std::string::npos);

	const auto read = call("desc.read", json::object{ { "path", "res://materials/red.mat.desc" } });
	BOOST_REQUIRE_MESSAGE(read.ok, read.error);
	BOOST_TEST(std::string(read.value.at("type").as_string()) == std::string("material_desc"));
	BOOST_TEST(std::string(read.value.at("version").as_string()) == std::string(made.value.at("version").as_string()));

	const auto changed = call("desc.write", write_args("materials/red.mat.desc", std::string(read.value.at("version").as_string()),
		R"({"__type":"material_desc","queue":"opaque","defines":["B","C"]})"));
	BOOST_REQUIRE_MESSAGE(changed.ok, changed.error);
	BOOST_TEST(std::string(changed.value.at("version").as_string()) != std::string(made.value.at("version").as_string()));
	const std::string on_disk = read_file(res_root / "materials" / "red.mat.desc");
	BOOST_TEST(on_disk.find("opaque") != std::string::npos);
	BOOST_TEST(on_disk.find("\"B\"") != std::string::npos);
	BOOST_TEST(on_disk.find("\"A\"") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnAgentCreatesALevel_ItsTypeHasNoSchemaYetSoItIsWarnedAbout)
{
	fs::create_directories(res_root / "levels");
	const auto made = call("desc.create", desc_args("levels/arena.desc", R"({"__type":"level_desc","name":"Arena","worlds":[]})"));
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	BOOST_REQUIRE_EQUAL(made.value.at("warnings").as_array().size(), 1u);
	BOOST_TEST(std::string(made.value.at("warnings").as_array()[0].at("kind").as_string()) == std::string("schema"));
	BOOST_TEST(read_file(res_root / "levels" / "arena.desc").find("\"Arena\"") != std::string::npos);

	const auto broken = call("desc.create", desc_args("levels/broken.desc", R"({"__type":"level_desc","worlds":5})"));
	BOOST_TEST(broken.code == cmd::codes::invalid_args);
	BOOST_TEST(!fs::exists(res_root / "levels" / "broken.desc"));
}

BOOST_AUTO_TEST_CASE(AContentThatBreaksItsSchema_IsRefusedWithThePointer_AndNothingChanges)
{
	const auto unknown_field = call("desc.create", desc_args("materials/a.mat.desc", R"({"__type":"material_desc","bogus":1})"));
	BOOST_TEST(unknown_field.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(unknown_field.value.is_object());
	const json::value& first = unknown_field.value.at("errors").as_array()[0];
	BOOST_TEST(std::string(first.at("json_path").as_string()) == std::string("/bogus"));
	BOOST_TEST(std::string(first.at("resource").as_string()) == std::string("res://materials/a.mat.desc"));
	BOOST_TEST(std::string(first.at("kind").as_string()) == std::string("schema"));
	BOOST_TEST(!fs::exists(res_root / "materials" / "a.mat.desc"));

	const auto wrong_type = call("desc.create", desc_args("materials/b.mat.desc", R"({"__type":"material_desc","defines":5})"));
	BOOST_TEST(wrong_type.code == cmd::codes::invalid_args);
	BOOST_TEST(std::string(wrong_type.value.at("errors").as_array()[0].at("json_path").as_string()) == std::string("/defines"));

	make_material("materials/c.mat.desc");
	const std::string before = read_file(res_root / "materials" / "c.mat.desc");
	const auto refused = call("desc.write", write_args("materials/c.mat.desc", version_of("materials/c.mat.desc"),
		R"({"__type":"material_desc","bogus":true})"));
	BOOST_TEST(refused.code == cmd::codes::invalid_args);
	BOOST_TEST(read_file(res_root / "materials" / "c.mat.desc") == before);
}

BOOST_AUTO_TEST_CASE(AnUnknownTypeOrAMissingHeaderOrAWrongVersion_IsRefused)
{
	for (const char* content : { R"({"__type":"no_such_desc"})", R"({"name":"x"})", R"({"__type":5})",
		R"({"__type":"material_desc","__version":99})", R"({"__type":"material_desc","queue":"mix","extra":{"__type":"no_such_desc"}})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/x.mat.desc", content));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "x.mat.desc"));
		}
	}
	const auto not_an_object = call("desc.create", json::object{ { "path", "materials/x.mat.desc" }, { "desc", json::array{} } });
	BOOST_TEST(!not_an_object.ok);
}

BOOST_AUTO_TEST_CASE(ANestedTypedObjectIsChecked_AtAnyDepth)
{
	const auto refused = call("desc.create", desc_args("materials/n.mat.desc",
		R"({"__type":"material_desc","extra":[{"__type":"material_desc","bogus":1}]})"));
	BOOST_TEST(refused.code == cmd::codes::invalid_args);
	bool pointed = false;
	for (const json::value& error : refused.value.at("errors").as_array()) {
		if (error.at("json_path").as_string() == "/extra/0/bogus") pointed = true;
	}
	BOOST_TEST(pointed);
}

BOOST_AUTO_TEST_CASE(PathsOutsideTheProject_OrNotDescFiles_AreRefusedBeforeTheDisk)
{
	for (const char* path : { "../escape.desc", "materials/../../escape.desc", "/abs/x.desc", "C:/x.desc", "http://x/y.desc",
		"materials\\x.desc", "materials/x.json", "materials/x.png", "materials/.desc", "", "materials/con.desc", "code://x.desc", "memory://x.desc" }) {
		BOOST_TEST_CONTEXT(path) {
			for (const char* id : { "desc.create", "desc.read", "desc.write" }) {
				json::object args{ { "path", path } };
				if (std::string_view(id) != "desc.read") args["desc"] = json::parse(k_material);
				if (std::string_view(id) == "desc.write") args["version"] = "v";
				const auto refused = call(id, args);
				BOOST_TEST(!refused.ok);
			}
		}
	}
	BOOST_TEST(!fs::exists(dir.path() / "escape.desc"));
}

BOOST_AUTO_TEST_CASE(AResourceSidecar_IsRefusedByEveryCommand_InAnyLetterCase)
{
	for (const char* path : { "materials/a.mat.desc.resource", "materials/A.MAT.DESC.Resource", "x.resource" }) {
		BOOST_TEST_CONTEXT(path) {
			for (const char* id : { "desc.create", "desc.read", "desc.write" }) {
				json::object args{ { "path", path } };
				if (std::string_view(id) != "desc.read") args["desc"] = json::parse(k_material);
				if (std::string_view(id) == "desc.write") args["version"] = "v";
				const auto refused = call(id, args);
				BOOST_TEST(refused.code == cmd::codes::invalid_args);
				BOOST_TEST(refused.error.find(".resource") != std::string::npos);
			}
		}
	}
	BOOST_TEST(!fs::exists(res_root / "materials" / "a.mat.desc.resource"));
	BOOST_TEST(!fs::exists(res_root / "x.resource"));
}

BOOST_AUTO_TEST_CASE(AGuidInTheContent_IsRefused_AndAWriteKeepsTheGuidOfTheFile)
{
	const auto at_root = call("desc.create", desc_args("materials/g.mat.desc", R"({"__type":"material_desc","__guid":"g"})"));
	BOOST_TEST(at_root.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(at_root.value.is_object());
	BOOST_TEST(text_of(at_root.value.at("errors").as_array()[0].at("json_path")) == "/__guid");
	BOOST_TEST(!fs::exists(res_root / "materials" / "g.mat.desc"));

	const auto nested = call("desc.create", desc_args("materials/n.mat.desc", R"({"__type":"material_desc","defines":[{"__guid":"g"}]})"));
	BOOST_TEST(nested.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(nested.value.is_object());
	BOOST_TEST(text_of(nested.value.at("errors").as_array()[0].at("json_path")) == "/defines/0/__guid");

	// A file written before the project's migration (EPIC-57 section 6.1) carries its ID as __guid: a write keeps it.
	std::ofstream(res_root / "materials" / "kept.mat.desc", std::ios::binary)
		<< R"({"__type":"material_desc","__guid":"11111111-2222-4333-8444-555555555555","queue":"opaque"})";
	const auto written = call("desc.write", write_args("materials/kept.mat.desc", version_of("materials/kept.mat.desc"),
		R"({"__type":"material_desc","queue":"mix"})"));
	BOOST_REQUIRE_MESSAGE(written.ok, written.error);
	const json::value on_disk = json::parse(read_file(res_root / "materials" / "kept.mat.desc"));
	BOOST_TEST(text_of(on_disk.at("__guid")) == "11111111-2222-4333-8444-555555555555");
	BOOST_TEST(text_of(on_disk.at("queue")) == "mix");
}

BOOST_AUTO_TEST_CASE(Create_NeverReplacesAFile_AlsoOneInALowerLayer)
{
	make_material("materials/once.mat.desc");
	const std::string before = read_file(res_root / "materials" / "once.mat.desc");
	const auto again = call("desc.create", desc_args("materials/once.mat.desc", R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST(again.code == cmd::codes::needs_input);
	BOOST_TEST(read_file(res_root / "materials" / "once.mat.desc") == before);

	dir.write("lower/materials/engine.mat.desc", k_material);
	const auto shadow = call("desc.create", desc_args("materials/engine.mat.desc", k_material));
	BOOST_TEST(shadow.code == cmd::codes::needs_input);
	BOOST_TEST(!fs::exists(res_root / "materials" / "engine.mat.desc"));
}

BOOST_AUTO_TEST_CASE(Write_RefusesAMissingFile_AndOneOfALowerLayer)
{
	const auto missing = call("desc.write", write_args("materials/none.mat.desc", "v", k_material));
	BOOST_TEST(missing.code == cmd::codes::not_found);

	dir.write("lower/materials/engine.mat.desc", k_material);
	const auto lower_only = call("desc.write", write_args("materials/engine.mat.desc", "v", k_material));
	BOOST_TEST(lower_only.code == cmd::codes::invalid_args);
	BOOST_TEST(!fs::exists(res_root / "materials" / "engine.mat.desc"));
	BOOST_TEST(read_file(lower / "materials" / "engine.mat.desc") == k_material);
}

BOOST_AUTO_TEST_CASE(Write_WithAnotherVersion_IsStale_AndTheFileStays)
{
	make_material("materials/s.mat.desc");
	const std::string version = version_of("materials/s.mat.desc");
	dir.write("res/materials/s.mat.desc", R"({"__type":"material_desc","queue":"opaque"})");
	const std::string changed_behind = read_file(res_root / "materials" / "s.mat.desc");

	const auto stale = call("desc.write", write_args("materials/s.mat.desc", version, k_material));
	BOOST_TEST(stale.code == cmd::codes::stale);
	BOOST_TEST(std::string(stale.value.at("version").as_string()) != std::string(version));
	BOOST_TEST(read_file(res_root / "materials" / "s.mat.desc") == changed_behind);
}

BOOST_AUTO_TEST_CASE(TheVersionIsCheckedAgainAtTheWrite_AfterThePersonSawTheQuestion)
{
	make_material("materials/q.mat.desc");
	const std::string version = version_of("materials/q.mat.desc");
	const cmd::call request = call_of(write_args("materials/q.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));

	const std::optional<std::string> question = commands.find("desc.write")->describe(request);
	BOOST_REQUIRE(question.has_value());
	BOOST_TEST(question->rfind("This request will be refused", 0) != 0u);

	dir.write("res/materials/q.mat.desc", R"({"__type":"material_desc","defines":["changed meanwhile"]})");
	const std::string behind = read_file(res_root / "materials" / "q.mat.desc");
	const auto answered = commands.invoke("desc.write", request);
	BOOST_TEST(answered.code == cmd::codes::stale);
	BOOST_TEST(read_file(res_root / "materials" / "q.mat.desc") == behind);
}

// #943: the type shown is the one the file on disk names, which anyone may have written: it stays on its line.
BOOST_AUTO_TEST_CASE(TheQuestion_KeepsATypeFromTheFileOnOneLine)
{
	dir.write("res/materials/forged.mat.desc", R"({"__type":"material_desc\nThe old content is kept in a backup.","queue":"opaque"})");
	const cmd::call request = call_of(write_args("materials/forged.mat.desc", version_of("materials/forged.mat.desc"), R"({"__type":"material_desc","queue":"opaque"})"));

	const std::optional<std::string> question = commands.find("desc.write")->describe(request);

	BOOST_REQUIRE(question.has_value());
	BOOST_TEST(question->find("\nThe old content is kept in a backup.") == std::string::npos);
	BOOST_TEST(question->find("material_desc\\nThe old content is kept in a backup.") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheQuestion_NamesTheFileTheSizesAndTheHotReload)
{
	make_material("materials/d.mat.desc");
	const cmd::call request = call_of(write_args("materials/d.mat.desc", version_of("materials/d.mat.desc"), R"({"__type":"material_desc","queue":"opaque"})"));
	const std::optional<std::string> question = commands.find("desc.write")->describe(request);
	BOOST_REQUIRE(question.has_value());
	BOOST_TEST(question->find("res://materials/d.mat.desc") != std::string::npos);
	BOOST_TEST(question->find("material_desc") != std::string::npos);
	BOOST_TEST(question->find("bytes now") != std::string::npos);
	BOOST_TEST(question->find("hot reload") != std::string::npos);
	BOOST_TEST(question->find("undo") != std::string::npos);

	const cmd::call bad = call_of(write_args("materials/d.mat.desc", "wrong", k_material));
	BOOST_TEST(commands.find("desc.write")->describe(bad)->rfind("This request will be refused", 0) == 0u);
	const cmd::call invalid = call_of(write_args("materials/d.mat.desc", version_of("materials/d.mat.desc"), R"({"__type":"material_desc","bogus":1})"));
	BOOST_TEST(commands.find("desc.write")->describe(invalid)->rfind("This request will be refused", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(TheFileOfTheOpenLevel_AndItsWorldFiles_AreBusy)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":["res://worlds/main.desc"]})");
	dir.write("res/worlds/main.desc", R"({"__type":"world_desc"})");
	dir.write("res/worlds/other.desc", R"({"__type":"world_desc"})");
	open_level = { "levels/open.desc" };
	const std::string level_before = read_file(res_root / "levels" / "open.desc");
	const std::string world_before = read_file(res_root / "worlds" / "main.desc");

	const auto level = call("desc.write", write_args("levels/open.desc", version_of("levels/open.desc"), R"({"__type":"level_desc","name":"x","worlds":[]})"));
	BOOST_TEST(level.code == cmd::codes::busy);
	BOOST_TEST(read_file(res_root / "levels" / "open.desc") == level_before);

	const auto world = call("desc.write", write_args("worlds/main.desc", version_of("worlds/main.desc"), R"({"__type":"world_desc"})"));
	BOOST_TEST(world.code == cmd::codes::busy);
	BOOST_TEST(read_file(res_root / "worlds" / "main.desc") == world_before);

	// A file the open level does not use is written as usual.
	const auto other = call("desc.write", write_args("worlds/other.desc", version_of("worlds/other.desc"), R"({"__type":"world_desc"})"));
	BOOST_TEST_MESSAGE(other.error);
	BOOST_TEST(other.code != cmd::codes::busy);

	const cmd::call request = call_of(write_args("levels/open.desc", "v", R"({"__type":"level_desc"})"));
	BOOST_TEST(commands.find("desc.write")->describe(request)->rfind("This request will be refused", 0) == 0u);
	const cmd::call world_request = call_of(write_args("worlds/main.desc", "v", R"({"__type":"world_desc"})"));
	BOOST_TEST(commands.find("desc.write")->describe(world_request)->rfind("This request will be refused", 0) == 0u);
}

// While a migration runs, or after one stopped half way until it is rolled back (#761), nothing an agent writes reaches
// the project: busy with the editor's reason, and the question says the request will be refused.
BOOST_AUTO_TEST_CASE(WhileTheProjectMayNotBeWritten_CreateAndWriteAreBusy_WithItsReason)
{
	make_material("materials/s.mat.desc");
	const std::string version = version_of("materials/s.mat.desc");
	const std::string before = read_file(res_root / "materials" / "s.mat.desc");
	blocked = "a migration of the project stopped half way: roll the migration back first";

	const auto written = call("desc.write", write_args("materials/s.mat.desc", version, k_material));
	BOOST_TEST(written.code == cmd::codes::busy);
	BOOST_TEST(written.error.find(*blocked) != std::string::npos);
	BOOST_TEST(read_file(res_root / "materials" / "s.mat.desc") == before);
	const cmd::call request = call_of(write_args("materials/s.mat.desc", version, k_material));
	BOOST_TEST(commands.find("desc.write")->describe(request)->rfind("This request will be refused", 0) == 0u);

	const auto made = call("desc.create", desc_args("materials/n.mat.desc", k_material));
	BOOST_TEST(made.code == cmd::codes::busy);
	BOOST_TEST(made.error.find(*blocked) != std::string::npos);
	BOOST_TEST(!fs::exists(res_root / "materials" / "n.mat.desc"));

	blocked.reset();
	make_material("materials/n.mat.desc");
}

// #991: a refusal is escaped once. The editor's reason already carries escapes (a migration id through escaped_text):
// they show as they are; the path the agent sent is escaped where the refusal is built, never twice.
BOOST_AUTO_TEST_CASE(TheQuestion_ShowsAnEscapeOfARefusalOnce)
{
	const cmd::call missing = call_of(write_args("materials/a\xE2\x80\xAE" "b.mat.desc", "v", k_material));
	const std::string missing_text = *commands.find("desc.write")->describe(missing);
	BOOST_TEST(missing_text.find("res://materials/a\\u{202E}b.mat.desc") != std::string::npos, missing_text);
	BOOST_TEST(missing_text.find("\\\\u{202E}") == std::string::npos, missing_text);
	BOOST_TEST(missing_text.find("\xE2\x80\xAE") == std::string::npos, missing_text);

	make_material("materials/s.mat.desc");
	blocked = "run editor --migrate-rollback a\\u{202E}b first";
	const cmd::call request = call_of(write_args("materials/s.mat.desc", version_of("materials/s.mat.desc"), k_material));
	const std::string blocked_text = *commands.find("desc.write")->describe(request);
	BOOST_TEST(blocked_text.find("--migrate-rollback a\\u{202E}b first") != std::string::npos, blocked_text);
	BOOST_TEST(blocked_text.find("\\\\u{202E}") == std::string::npos, blocked_text);
	blocked.reset();
}

// #920: a world the open level names as {guid, path} is the level's as much as one named by a path.
BOOST_AUTO_TEST_CASE(AWorldFileTheOpenLevelNamesAsAGuidAndAPath_IsBusyToo)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	dir.write("res/levels/open.desc",
		R"({"__type":"level_desc","name":"o","worlds":[{"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","path":"res://worlds/main.desc"},{"path":"res://worlds/second.desc"}]})");
	dir.write("res/worlds/main.desc", R"({"__type":"world_desc"})");
	dir.write("res/worlds/second.desc", R"({"__type":"world_desc"})");
	open_level = { "levels/open.desc" };

	for (const char* world : { "worlds/main.desc", "worlds/second.desc" }) {
		BOOST_TEST_CONTEXT(world) {
			const std::string before = read_file(res_root / world);
			const auto written = call("desc.write", write_args(world, version_of(world), R"({"__type":"world_desc"})"));
			BOOST_TEST(written.code == cmd::codes::busy);
			BOOST_TEST(read_file(res_root / world) == before);
		}
	}
	// A name no file has yet, which the level lists: a new file there would be the level's.
	dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":[{"path":"res://worlds/planned.desc"}]})");
	const auto created = call("desc.create", desc_args("worlds/planned.desc", R"({"__type":"world_desc"})"));
	BOOST_TEST(created.code == cmd::codes::busy);
	BOOST_TEST(!fs::exists(res_root / "worlds" / "planned.desc"));
}

// #920: the index moved the guid, the level's `path` is stale: where the world is now and what the path names are both busy.
BOOST_AUTO_TEST_CASE(AWorldTheIndexMoved_IsBusyAtItsNewPlaceAndAtItsStalePath)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	dir.write("res/levels/open.desc",
		R"({"__type":"level_desc","name":"o","worlds":[{"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","path":"res://worlds/stale.desc"}]})");
	dir.write("res/worlds/moved.desc", R"({"__type":"world_desc"})");
	dir.write("res/worlds/stale.desc", R"({"__type":"world_desc"})");
	open_level = { "levels/open.desc" };
	auto index = std::make_shared<test_util::asset_ids_index>();
	index->hold("0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6", "res://worlds/moved.desc");
	const test_util::asset_index_scope installed{ *rs, index };

	for (const char* world : { "worlds/moved.desc", "worlds/stale.desc" }) {
		BOOST_TEST_CONTEXT(world) {
			const std::string before = read_file(res_root / world);
			const auto written = call("desc.write", write_args(world, version_of(world), R"({"__type":"world_desc"})"));
			BOOST_TEST(written.code == cmd::codes::busy);
			BOOST_TEST(read_file(res_root / world) == before);
		}
	}
	// The same two names with no file there: a new file at either would be the level's.
	for (const char* world : { "worlds/moved.desc", "worlds/stale.desc" }) {
		BOOST_TEST_CONTEXT(world) {
			fs::remove(res_root / world);
			const auto created = call("desc.create", desc_args(world, R"({"__type":"world_desc"})"));
			BOOST_TEST(created.code == cmd::codes::busy);
			BOOST_TEST(!fs::exists(res_root / world));
		}
	}
}

BOOST_AUTO_TEST_CASE(AnOpenLevelThatCannotBeReadOrParsed_MakesEveryWriteBusy)
{
	fs::create_directories(res_root / "levels");
	make_material("materials/m.mat.desc");
	const std::string version = version_of("materials/m.mat.desc");
	dir.write("res/levels/broken.desc", "{ this is not json");
	open_level = { "levels/broken.desc" };
	const std::string before = read_file(res_root / "materials" / "m.mat.desc");
	const auto busy = call("desc.write", write_args("materials/m.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST(busy.code == cmd::codes::busy);
	BOOST_TEST(read_file(res_root / "materials" / "m.mat.desc") == before);

	open_level = { "levels/not_there_yet.desc" };
	const auto free = call("desc.write", write_args("materials/m.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST_MESSAGE(free.error);
	BOOST_TEST(free.ok);
}

// #966: the resolver opens these tags as worlds/x.desc, the path rules of the guard reject them.
BOOST_AUTO_TEST_CASE(AWorldEntryTheGuardCannotRead_MakesEveryWriteBusy)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	make_material("materials/m.mat.desc");
	dir.write("res/worlds/x.desc", R"({"__type":"world_desc"})");
	std::string padded = "res://worlds/";
	while (padded.size() <= 520) padded += "./";
	padded += "x.desc";

	for (const std::string& entry : { std::string("res://worlds//x.desc"), std::string("res://worlds/./x.desc"), padded }) {
		BOOST_TEST_CONTEXT(entry) {
			open_level.clear();
			const std::string world_version = version_of("worlds/x.desc");
			const std::string material_version = version_of("materials/m.mat.desc");
			const std::string world_before = read_file(res_root / "worlds" / "x.desc");
			const std::string material_before = read_file(res_root / "materials" / "m.mat.desc");
			dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":[{"path":")" + entry + R"("}]})");
			open_level = { "levels/open.desc" };

			const auto world = call("desc.write", write_args("worlds/x.desc", world_version, R"({"__type":"world_desc"})"));
			BOOST_TEST(world.code == cmd::codes::busy);
			BOOST_TEST(read_file(res_root / "worlds" / "x.desc") == world_before);
			const auto other = call("desc.write", write_args("materials/m.mat.desc", material_version, R"({"__type":"material_desc","queue":"opaque"})"));
			BOOST_TEST(other.code == cmd::codes::busy);
			BOOST_TEST(says_which_entry(other.error));
			BOOST_TEST(read_file(res_root / "materials" / "m.mat.desc") == material_before);
			const auto created = call("desc.create", desc_args("materials/n.mat.desc", k_material));
			BOOST_TEST(created.code == cmd::codes::busy);
			BOOST_TEST(says_which_entry(created.error));
			BOOST_TEST(!fs::exists(res_root / "materials" / "n.mat.desc"));
		}
	}
	// The whole text, with the entry and the rule it breaks.
	dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":[{"path":"res://worlds//x.desc"}]})");
	const auto named = call("desc.create", desc_args("materials/n.mat.desc", k_material));
	BOOST_TEST(named.code == cmd::codes::busy);
	BOOST_TEST(named.error =="the open level 'levels/open.desc' has a worlds entry 'res://worlds//x.desc' that the path rules reject "
		"(the path must not have empty, '.' or '..' segments): desc files are not written until it is fixed or the level is closed");
	cmd::call asked = call_of(write_args("materials/m.mat.desc", "any", R"({"__type":"material_desc","queue":"opaque"})"));
	const std::optional<std::string> shown = commands.find("desc.write")->describe(asked);
	BOOST_REQUIRE(shown.has_value());
	BOOST_TEST(says_which_entry(*shown));
}

// #966: more spellings the path rules reject; whatever file each names, no desc file is written and the answer says why.
BOOST_AUTO_TEST_CASE(AnOddSpellingOfAWorldEntry_MakesEveryWriteBusy_AndSaysWhy)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	make_material("materials/m.mat.desc");
	dir.write("res/worlds/x.desc", R"({"__type":"world_desc"})");
	const std::string version = version_of("materials/m.mat.desc");
	const std::string before = read_file(res_root / "materials" / "m.mat.desc");

	for (const char* element : {
		R"({"path":"res://worlds/x.desc."})",
		R"({"path":"res://worlds/x.desc "})",
		R"({"path":"res://worlds/../worlds/x.desc"})",
		R"({"path":"res://worlds/x\u0000.desc"})",
		R"({"path":"res:/worlds/x.desc"})",
		R"("res://worlds/x.desc.")",
		R"({"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","path":"res://worlds//x.desc"})" }) {
		BOOST_TEST_CONTEXT(element) {
			dir.write("res/levels/open.desc", std::string(R"({"__type":"level_desc","name":"o","worlds":[)") + element + "]}");
			open_level = { "levels/open.desc" };
			const auto written = call("desc.write", write_args("materials/m.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));
			BOOST_TEST(written.code == cmd::codes::busy);
			BOOST_TEST(says_which_entry(written.error));
			BOOST_TEST(read_file(res_root / "materials" / "m.mat.desc") == before);
			const auto created = call("desc.create", desc_args("materials/n.mat.desc", k_material));
			BOOST_TEST(created.code == cmd::codes::busy);
			BOOST_TEST(says_which_entry(created.error));
			BOOST_TEST(!fs::exists(res_root / "materials" / "n.mat.desc"));
			open_level.clear();
		}
	}
}

// #966: res::tag turns `\` into `/`, so this entry is an ordinary one: its file is the level's, the others are not.
BOOST_AUTO_TEST_CASE(AWorldEntryWithABackslash_IsAnOrdinaryEntry)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	make_material("materials/m.mat.desc");
	dir.write("res/worlds/x.desc", R"({"__type":"world_desc"})");
	const std::string world_version = version_of("worlds/x.desc");
	const std::string material_version = version_of("materials/m.mat.desc");
	dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":[{"path":"res://worlds\\x.desc"}]})");
	open_level = { "levels/open.desc" };

	const auto world = call("desc.write", write_args("worlds/x.desc", world_version, R"({"__type":"world_desc"})"));
	BOOST_TEST(world.code == cmd::codes::busy);
	BOOST_TEST(world.error.find("belongs to the open level") != std::string::npos);
	const auto other = call("desc.write", write_args("materials/m.mat.desc", material_version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST_MESSAGE(other.error);
	BOOST_TEST(other.ok);
}

// #966: `RES` is another protocol to res::tag and no resolver serves it, so the entry names no file and blocks nothing.
BOOST_AUTO_TEST_CASE(AWorldEntryUnderAnUppercaseProtocol_NamesNoFile)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	make_material("materials/m.mat.desc");
	dir.write("res/worlds/x.desc", R"({"__type":"world_desc"})");
	const std::string world_version = version_of("worlds/x.desc");
	const std::string material_version = version_of("materials/m.mat.desc");
	dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":[{"path":"RES://worlds/x.desc"}]})");
	open_level = { "levels/open.desc" };

	const auto world = call("desc.write", write_args("worlds/x.desc", world_version, R"({"__type":"world_desc"})"));
	BOOST_TEST_MESSAGE(world.error);
	BOOST_TEST(world.ok);
	const auto other = call("desc.write", write_args("materials/m.mat.desc", material_version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST_MESSAGE(other.error);
	BOOST_TEST(other.ok);
}

// If this starts loading, the guard in edt_desc_file_commands.cpp (#966) has to treat such an entry like `res://`.
BOOST_AUTO_TEST_CASE(AnUppercaseProtocol_IsNotServedByTheResourceSystem_SoTheGuardSkipsIt)
{
	fs::create_directories(res_root / "worlds");
	dir.write("res/worlds/x.desc", R"({"__type":"world_desc"})");

	BOOST_TEST(rs->exists(res::tag{ "res://worlds/x.desc" }));
	const res::tag uppercase{ "RES://worlds/x.desc" };
	BOOST_TEST(uppercase.protocol() == "RES");
	BOOST_TEST(!rs->exists(uppercase));
}

// #966: the project's root may be the engine's base folder, so an engine:// world is compared as a file on disk.
BOOST_AUTO_TEST_CASE(AnEngineWorldInTheProjectsOwnFolder_IsTheLevels)
{
	fs::create_directories(res_root / "levels");
	fs::create_directories(res_root / "worlds");
	make_material("materials/m.mat.desc");
	dir.write("res/worlds/x.desc", R"({"__type":"world_desc"})");
	const std::string world_version = version_of("worlds/x.desc");
	const std::string material_version = version_of("materials/m.mat.desc");
	const std::string before = read_file(res_root / "worlds" / "x.desc");
	const engine_mount_scope engine{ *rs, res_root };
	dir.write("res/levels/open.desc",
		R"({"__type":"level_desc","name":"o","worlds":[{"path":"engine://worlds/x.desc"},{"path":"engine://worlds/planned.desc"}]})");
	open_level = { "levels/open.desc" };

	const auto world = call("desc.write", write_args("worlds/x.desc", world_version, R"({"__type":"world_desc"})"));
	BOOST_TEST(world.code == cmd::codes::busy);
	BOOST_TEST(world.error.find("belongs to the open level") != std::string::npos);
	BOOST_TEST(read_file(res_root / "worlds" / "x.desc") == before);
	const auto planned = call("desc.create", desc_args("worlds/planned.desc", R"({"__type":"world_desc"})"));
	BOOST_TEST(planned.code == cmd::codes::busy);
	BOOST_TEST(!fs::exists(res_root / "worlds" / "planned.desc"));
	const auto other = call("desc.write", write_args("materials/m.mat.desc", material_version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST_MESSAGE(other.error);
	BOOST_TEST(other.ok);
}

// #966: an agent cannot plant, in a level that is not open, a worlds entry the open-level guard could not read later.
BOOST_AUTO_TEST_CASE(ALevelWithAWorldEntryThePathRulesReject_IsNotWritten)
{
	fs::create_directories(res_root / "levels");
	const auto pointer_of = [](const cmd::result& refused, std::string_view pointer) {
		const json::value* errors = refused.value.is_object() ? refused.value.as_object().if_contains("errors") : nullptr;
		if (!errors || !errors->is_array()) return false;
		for (const json::value& error : errors->as_array()) {
			if (error.at("json_path").as_string() == pointer) return true;
		}
		return false;
	};
	const auto level_with = [](const char* element) {
		return std::string(R"({"__type":"level_desc","name":"n","worlds":[{"path":"res://worlds/a.desc"},)") + element + "]}";
	};
	const std::pair<const char*, const char*> cases[] = {
		{ R"("res://worlds//x.desc")", "/worlds/1" },
		{ R"("worlds//x.desc")", "/worlds/1" },
		{ R"({"path":"res://worlds//x.desc"})", "/worlds/1/path" },
		{ R"({"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","path":"res://worlds/./x.desc"})", "/worlds/1/path" },
		{ R"({"path":"worlds/x.desc."})", "/worlds/1/path" },
		{ R"("engine://worlds/x.desc")", "/worlds/1" },
	};
	for (const auto& [element, pointer] : cases) {
		BOOST_TEST_CONTEXT(element) {
			const std::string content = level_with(element);
			const auto created = call("desc.create", desc_args("levels/n.desc", content.c_str()));
			BOOST_TEST(created.code == cmd::codes::invalid_args);
			BOOST_TEST(pointer_of(created, pointer));
			BOOST_TEST(!fs::exists(res_root / "levels" / "n.desc"));
		}
	}
	dir.write("res/levels/old.desc", R"({"__type":"level_desc","name":"old","worlds":[]})");
	const std::string before = read_file(res_root / "levels" / "old.desc");
	const std::string content = level_with(R"("res://worlds//x.desc")");
	const auto written = call("desc.write", write_args("levels/old.desc", version_of("levels/old.desc"), content.c_str()));
	BOOST_TEST(written.code == cmd::codes::invalid_args);
	BOOST_TEST(pointer_of(written, "/worlds/1"));
	BOOST_TEST(read_file(res_root / "levels" / "old.desc") == before);

	const std::string fine = level_with(R"("worlds/b.desc")");
	const auto created = call("desc.create", desc_args("levels/n.desc", fine.c_str()));
	BOOST_TEST_MESSAGE(created.error);
	BOOST_TEST(created.ok);
}

// #966: memory:// has no folder a project file can be in, and an engine:// entry outside the project's folder is no project file.
BOOST_AUTO_TEST_CASE(AWorldUnderAnotherProtocol_BlocksNoWriteToTheProject)
{
	fs::create_directories(res_root / "levels");
	make_material("materials/m.mat.desc");
	const std::string version = version_of("materials/m.mat.desc");
	dir.write("res/levels/open.desc",
		R"({"__type":"level_desc","name":"o","worlds":[{"path":"engine://worlds/x.desc"},{"path":"memory://worlds/y.desc"}]})");
	open_level = { "levels/open.desc" };

	const auto written = call("desc.write", write_args("materials/m.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST_MESSAGE(written.error);
	BOOST_TEST(written.ok);
	const auto created = call("desc.create", desc_args("materials/n.mat.desc", k_material));
	BOOST_TEST_MESSAGE(created.error);
	BOOST_TEST(created.ok);
	BOOST_TEST(fs::exists(res_root / "materials" / "n.mat.desc"));
}

// A guid no file holds, without a path: the resolver loads nothing by it, so no file is the level's through it.
BOOST_AUTO_TEST_CASE(AWorldEntryThatNamesNothing_BlocksNothing)
{
	fs::create_directories(res_root / "levels");
	make_material("materials/m.mat.desc");
	const std::string version = version_of("materials/m.mat.desc");
	dir.write("res/levels/open.desc", R"({"__type":"level_desc","name":"o","worlds":[{"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6"}]})");
	open_level = { "levels/open.desc" };

	const auto written = call("desc.write", write_args("materials/m.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST_MESSAGE(written.error);
	BOOST_TEST(written.ok);
	const auto created = call("desc.create", desc_args("materials/n.mat.desc", k_material));
	BOOST_TEST_MESSAGE(created.error);
	BOOST_TEST(created.ok);
}

BOOST_AUTO_TEST_CASE(ATagThatLeavesTheProject_IsRefused_InParentAndInEveryTagField)
{
	for (const char* content : {
		R"({"__type":"material_desc","__parent":"user://secret.desc"})",
		R"({"__type":"material_desc","__parent":"edt://base.desc"})",
		R"({"__type":"material_desc","__parent":"../x.desc"})",
		R"({"__type":"material_desc","__parent":"user/xsave.desc"})",
		R"({"__type":"material_desc","__parent":"user:/xsave.desc"})",
		R"({"__type":"material_desc","__parent":"materials/other.mat.desc"})",
		R"({"__type":"material_desc","__parent":"res/other.mat.desc"})",
		R"({"__type":"material_desc","__parent":"res://a/../../x.desc"})",
		R"({"__type":"material_desc","__parent":"res://a/x.png"})",
		R"({"__type":"material_desc","__parent":5})",
		R"({"__type":"material_desc","shader_fragment":"user://x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"engine://x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"cache://x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"cache/xa.frag"})",
		R"({"__type":"material_desc","shader_fragment":"shaders/a.frag"})",
		R"({"__type":"material_desc","shader_fragment":"../../x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"C:/x.frag"})",
		R"({"__type":"material_desc","defines":["code://x"]})",
		R"({"__type":"material_desc","samplers":["../../x.png"]})",
		R"({"__type":"material_desc","samplers":["user://x.png"]})",
		R"({"__type":"material_desc","samplers":["engine/xx.png"]})",
		R"({"__type":"material_desc","samplers":["user:/xa.png"]})",
		R"({"__type":"material_desc","samplers":{"0":"user/xx.png"}})",
		R"({"__type":"material_desc","samplers":{"0":{"slot":["code/xa.png"]}}})",
		R"({"__type":"material_desc","defines":["user/x"]})",
		R"({"__type":"level_desc","worlds":["code/xa.desc"]})",
		R"({"__type":"level_desc","worlds":["user:/xa.desc"]})",
		R"({"__type":"level_desc","worlds":["memory://x"]})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/t.mat.desc", content));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "t.mat.desc"));
		}
	}
	for (const char* content : {
		R"({"__type":"material_desc","__parent":"res://base_material.desc"})",
		R"({"__type":"material_desc","__parent":"res://materials/other.mat.desc"})",
		R"({"__type":"material_desc","shader_fragment":"res://shaders/a.frag"})",
		R"({"__type":"material_desc","samplers":["res://textures/a.png",null]})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto made = call("desc.create", desc_args("materials/ok.mat.desc", content));
			BOOST_TEST_MESSAGE(made.error);
			BOOST_TEST(made.ok);
			fs::remove(res_root / "materials" / "ok.mat.desc");
		}
	}
}

BOOST_AUTO_TEST_CASE(AParentWrittenAsAReferenceObject_PassesTheSameChecksAsTheString)
{
	for (const char* content : {
		R"({"__type":"material_desc","__parent":{"path":"user://secret.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"edt://base.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"../x.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"user/xsave.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"user:/xsave.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"materials/other.mat.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"res/other.mat.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"res://a/../../x.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"res://a/x.png"}})",
		R"({"__type":"material_desc","__parent":{"path":""}})",
		R"({"__type":"material_desc","__parent":{"path":5}})",
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"user://secret.desc"}})",
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"}})",
		R"({"__type":"material_desc","__parent":{"guid":"not-a-uuid","path":"res://base_material.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"res://base_material.desc","__type":"material_desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"res://base_material.desc","other":"user://secret.desc"}})",
		R"({"__type":"material_desc","__parent":{}})",
		R"({"__type":"material_desc","__parent":["res://base_material.desc"]})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/t.mat.desc", content));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "t.mat.desc"));
		}
	}
	for (const char* content : {
		R"({"__type":"material_desc","__parent":{"path":"res://base_material.desc"}})",
		R"({"__type":"material_desc","__parent":{"path":"res://materials/other.mat.desc"}})",
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://base_material.desc"}})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto made = call("desc.create", desc_args("materials/ok.mat.desc", content));
			BOOST_TEST_MESSAGE(made.error);
			BOOST_TEST(made.ok);
			fs::remove(res_root / "materials" / "ok.mat.desc");
		}
	}

	make_material("materials/w.mat.desc");
	const std::string before = read_file(res_root / "materials" / "w.mat.desc");
	const auto guid_only = call("desc.write", write_args("materials/w.mat.desc", version_of("materials/w.mat.desc"),
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"}})"));
	BOOST_TEST(guid_only.code == cmd::codes::invalid_args);
	BOOST_TEST(guid_only.error.find("a \"guid\" alone is not accepted here") != std::string::npos, guid_only.error);
	BOOST_TEST(read_file(res_root / "materials" / "w.mat.desc") == before);
}

BOOST_AUTO_TEST_CASE(AnUnknownKeyWithTwoUnderscores_IsRefused_SoNoObjectHidesFromTheCheck)
{
	const auto hidden = call("desc.create", desc_args("materials/h.mat.desc", R"({"__type":"material_desc","__x":{"__type":"no_such_desc"}})"));
	BOOST_TEST(hidden.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(hidden.value.is_object());
	BOOST_TEST(std::string(hidden.value.at("errors").as_array()[0].at("json_path").as_string()) == std::string("/__x"));
	BOOST_TEST(!fs::exists(res_root / "materials" / "h.mat.desc"));

	const auto kept = call("desc.create", desc_args("materials/k.mat.desc", R"({"__type":"material_desc","__id":3})"));
	BOOST_TEST_MESSAGE(kept.error);
	BOOST_TEST(kept.ok);
}

BOOST_AUTO_TEST_CASE(ManyProblems_AreCappedWithoutLettingTheFilePass)
{
	std::string worlds;
	for (int i = 0; i < 30; ++i) worlds += std::string(i ? "," : "") + R"({"__type":"material_desc","bogus":1})";
	const std::string content = R"({"__type":"level_desc","worlds":[)" + worlds + "]}";
	const std::vector<res::diagnostic> found = edt::validate_desc_tree(*descs, json::parse(content), "res://levels/x.desc").problems;
	std::size_t errors = 0;
	std::size_t warnings = 0;
	for (const res::diagnostic& item : found) (item.severity == res::diagnostic_severity::error ? errors : warnings)++;
	BOOST_TEST(errors == 20u);
	BOOST_TEST(warnings == 1u);

	fs::create_directories(res_root / "levels");
	const auto refused = call("desc.create", json::object{ { "path", "levels/many.desc" }, { "desc", json::parse(content) } });
	BOOST_TEST(refused.code == cmd::codes::invalid_args);
	BOOST_TEST(!fs::exists(res_root / "levels" / "many.desc"));
}

BOOST_AUTO_TEST_CASE(TheActionNamesOfInputActions_AreNotTags_ButAnotherProtocolIsStillRefused)
{
	const std::string actions = R"({"__type":"input_actions_desc","actions":[
		{"name":"MoveLeft","bindings":[{"key":"A"},{"key":"LEFT"}]},
		{"name":"Fire","bindings":[{"mouse":"LEFT"}]}]})";
	std::size_t errors = 0;
	for (const res::diagnostic& item : edt::validate_desc_tree(*descs, json::parse(actions), "res://input/actions.desc").problems) {
		if (item.severity == res::diagnostic_severity::error) ++errors;
	}
	BOOST_TEST(errors == 0u);

	for (const std::string_view bad : { "user/x", "user://x", "edt://settings/a", "https://example.com/a" }) {
		BOOST_TEST_CONTEXT(bad) {
			const json::value with_bad = json::parse(std::string(R"({"__type":"input_actions_desc","actions":[{"name":")") + std::string(bad)
				+ R"(","bindings":[{"key":"A"}]}]})");
			bool refused = false;
			for (const res::diagnostic& item : edt::validate_desc_tree(*descs, with_bad, "res://input/actions.desc").problems) {
				if (item.severity == res::diagnostic_severity::error) refused = true;
			}
			BOOST_TEST(refused);
		}
	}

	fs::create_directories(res_root / "input");
	const auto created = call("desc.create", json::object{ { "path", "input/actions.desc" }, { "desc", json::parse(actions) } });
	BOOST_TEST_MESSAGE(created.error);
	BOOST_TEST(created.ok);
}

BOOST_AUTO_TEST_CASE(ALinkUnderRes_IsNeitherReadNorWritten_NorALinkedFolder)
{
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside / "folder");
	dir.write("outside/target.mat.desc", k_material);
	std::error_code ec;
	fs::create_symlink(outside / "target.mat.desc", res_root / "materials" / "link.mat.desc", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("no symlinks here: " << ec.message());
		return;
	}
	fs::create_directory_symlink(outside / "folder", res_root / "linked", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("no folder links here: " << ec.message());
		return;
	}
	const std::string target_before = read_file(outside / "target.mat.desc");

	BOOST_TEST(call("desc.read", json::object{ { "path", "materials/link.mat.desc" } }).code == cmd::codes::invalid_args);
	BOOST_TEST(call("desc.write", write_args("materials/link.mat.desc", "v", k_material)).code == cmd::codes::invalid_args);
	BOOST_TEST(call("desc.create", desc_args("materials/link.mat.desc", k_material)).code == cmd::codes::invalid_args);
	BOOST_TEST(read_file(outside / "target.mat.desc") == target_before);

	BOOST_TEST(call("desc.create", desc_args("linked/new.mat.desc", k_material)).code == cmd::codes::invalid_args);
	BOOST_TEST(!fs::exists(outside / "folder" / "new.mat.desc"));
	dir.write("outside/folder/in.mat.desc", k_material);
	BOOST_TEST(call("desc.read", json::object{ { "path", "linked/in.mat.desc" } }).code == cmd::codes::invalid_args);
	BOOST_TEST(call("desc.write", write_args("linked/in.mat.desc", "v", k_material)).code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(ANonDescPath_IsRefusedWithATextAboutDescFiles)
{
	const auto refused = call("desc.read", json::object{ { "path", "materials/x.json" } });
	BOOST_TEST(!refused.ok);
	BOOST_TEST(refused.error.find("desc file") != std::string::npos);
	BOOST_TEST(refused.error.find("level file") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ABigFile_IsNotReadNorWritten)
{
	dir.write("res/materials/big.mat.desc", std::string(edt::k_desc_file_limit + 1, ' '));
	const auto read = call("desc.read", json::object{ { "path", "materials/big.mat.desc" } });
	BOOST_TEST(read.code == cmd::codes::failed);

	std::string huge = R"({"__type":"material_desc","defines":[)";
	for (std::size_t size = 0; size < edt::k_desc_file_limit; size += 8) huge += "\"abcdef\",";
	huge += "\"z\"]}";
	const auto too_big = call("desc.create", json::object{ { "path", "materials/huge.mat.desc" }, { "desc", json::parse(huge) } });
	BOOST_TEST(!too_big.ok);
	BOOST_TEST(!fs::exists(res_root / "materials" / "huge.mat.desc"));
}

BOOST_AUTO_TEST_CASE(AFileThatIsNoUtf8_IsNotRead)
{
	dir.write("res/materials/raw.mat.desc", "{\"a\":\"\xff\xfe\"}");
	BOOST_TEST(call("desc.read", json::object{ { "path", "materials/raw.mat.desc" } }).code == cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(TheWritePolicyOfTheResolver_RefusesAndTheReasonIsShown)
{
	rs->unregistrate_resolver(res::tag::default_protocol());
	rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_root },
		res::mount_access::read_write, res::write_policy(res::tag::default_protocol(), { "res://levels/**" }));
	const auto refused = call("desc.create", desc_args("materials/denied.mat.desc", k_material));
	BOOST_TEST(refused.code == cmd::codes::failed);
	const std::string marker = "was not created: ";
	const std::size_t at = refused.error.find(marker);
	BOOST_REQUIRE(at != std::string::npos);
	BOOST_TEST(refused.error.size() > at + marker.size());
	BOOST_TEST(!fs::exists(res_root / "materials" / "denied.mat.desc"));
}

BOOST_AUTO_TEST_CASE(AWrite_SignalsTheChange_SoTheHotReloadSeesIt)
{
	make_material("materials/h.mat.desc");
	int reloads = 0;
	int owner = 0;
	rs->watch(res::tag("res://materials/h.mat.desc"), &owner, [&reloads](const res::tag&) { ++reloads; });
	const auto changed = call("desc.write", write_args("materials/h.mat.desc", version_of("materials/h.mat.desc"), R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_REQUIRE_MESSAGE(changed.ok, changed.error);
	tasks.drain_main();
	BOOST_TEST(reloads >= 1);
	rs->unwatch(res::tag("res://materials/h.mat.desc"), &owner);
}

BOOST_AUTO_TEST_CASE(AnAgentsChange_IsAnnounced)
{
	make_material("materials/e.mat.desc");
	BOOST_REQUIRE_EQUAL(actions.size(), 1u);
	BOOST_TEST(actions[0].command == "desc.create");
	BOOST_TEST(actions[0].target == "materials/e.mat.desc");
}

// #631: the checks read a protocol exactly as res::tag does (one rule, res::tag::protocol_of), and a text res::tag gives
// any protocol is refused unless it is a good res:// tag: as a __parent and as a plain string.
BOOST_AUTO_TEST_CASE(TheChecksReadTheProtocolAsResTagDoes_AndRefuseEveryTextWithAnotherOne)
{
	const auto errors_of = [&](const json::value& content) {
		std::size_t errors = 0;
		for (const res::diagnostic& item : edt::validate_desc_tree(*descs, content, "res://materials/t.mat.desc").problems) {
			if (item.severity == res::diagnostic_severity::error) ++errors;
		}
		return errors;
	};
	constexpr const char* good = "res://materials/ok.mat.desc";
	for (const std::string_view text : { "a:b://x", "://x", "res://", "res:///abs", "RES://a.png", "\\\\", "c:\\\\path",
		"user/xsecret.desc", "user:/xa.png", "user\\xa.png", "res://res://a.png", "res://user://a.png", "x-y://a",
		"materials/a.mat.desc", "res:/a.png", "res/a.png", good }) {
		BOOST_TEST_CONTEXT(text) {
			const res::tag tag{ text };
			BOOST_TEST(edt::tag_protocol(edt::tag_text_normalized(text)) == tag.protocol());
			const json::object as_parent{ { "__type", "material_desc" }, { "__parent", text } };
			const json::object as_text{ { "__type", "material_desc" }, { "defines", json::array{ json::string(text) } } };
			if (text == good) {
				BOOST_TEST(errors_of(as_parent) == 0u);
				BOOST_TEST(errors_of(as_text) == 0u);
			} else {
				BOOST_TEST(errors_of(as_parent) > 0u);
				if (!tag.protocol().empty()) {
					BOOST_TEST(errors_of(as_text) > 0u);
				}
			}
		}
	}
	const json::object plain{ { "__type", "material_desc" }, { "defines", json::array{ "materials/a.mat.desc" } } };
	BOOST_TEST(errors_of(plain) == 0u);
}

BOOST_AUTO_TEST_CASE(ATagInsideAMapOfTags_FollowsTheStrictRuleToo)
{
	for (const char* content : {
		R"({"__type":"test.texture_table","textures":{"a":"user://x.png"}})",
		R"({"__type":"test.texture_table","textures":{"a":"res://ok.png","b":"edt://x.png"}})",
		R"({"__type":"test.texture_table","textures":{"a":"textures/no_protocol.png"}})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/t.desc", content));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "t.desc"));
		}
	}
	const auto fine = call("desc.create", desc_args("materials/t.desc", R"({"__type":"test.texture_table","textures":{"a":"res://ok.png"}})"));
	BOOST_TEST_MESSAGE(fine.error);
	BOOST_TEST(fine.ok);
}

namespace {

// An asset index that knows where some IDs are and keys no tag by an ID, so no cache entry moves.
struct paths_index final : res::asset_index
{
	std::map<res::asset_id, res::tag> paths;
	std::optional<res::asset_id> id_of(const res::tag&) const override { return std::nullopt; }
	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		const auto found = paths.find(id);
		return found == paths.end() ? std::nullopt : std::optional{ found->second };
	}
};

}

// #773: a tag field takes a {guid, path} reference (EPIC-57 section 4.4): its path follows the strict rule, it has no
// other key, its guid is a UUID that is not nil. A guid alone is refused, whether a file holds it or not.
BOOST_AUTO_TEST_CASE(AReferenceObject_InATagField_FollowsTheRuleOfATag)
{
	const std::string guid = R"("guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f")";
	for (const std::string& content : {
		R"({"__type":"material_desc","shader_fragment":{)" + guid + R"(,"path":"user://x.frag"}})",
		R"({"__type":"material_desc","shader_fragment":{)" + guid + R"(,"path":"shaders/a.frag"}})",
		R"({"__type":"material_desc","shader_fragment":{)" + guid + R"(,"path":"res://shaders/a.frag","extra":1}})",
		std::string(R"({"__type":"material_desc","shader_fragment":{"guid":"00000000-0000-0000-0000-000000000000"}})"),
		R"({"__type":"material_desc","shader_fragment":{)" + guid + R"(,"path":5}})",
		R"({"__type":"material_desc","shader_fragment":{)" + guid + "}}",
		R"({"__type":"test.texture_table","textures":{"a":{)" + guid + R"(,"path":"edt://x.png"}}})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/r.mat.desc", content.c_str()));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "r.mat.desc"));
		}
	}
	for (const std::string& content : {
		R"({"__type":"material_desc","shader_fragment":{)" + guid + R"(,"path":"res://shaders/a.frag"}})",
		R"({"__type":"test.texture_table","textures":{"a":{)" + guid + R"(,"path":"res://ok.png"}}})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto made = call("desc.create", desc_args("materials/r.mat.desc", content.c_str()));
			BOOST_TEST_MESSAGE(made.error);
			BOOST_TEST(made.ok);
			fs::remove(res_root / "materials" / "r.mat.desc");
		}
	}

	// With an asset index a guid alone is refused the same way, known or not.
	auto index = std::make_shared<paths_index>();
	index->paths.emplace(*res::asset_id_from_text("0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6"), res::tag{ "res://shaders/b.frag" });
	rs->set_asset_index(index);
	const std::string unknown_guid = R"({"__type":"material_desc","shader_fragment":{)" + guid + "}}";
	const auto unknown = call("desc.create", desc_args("materials/r.mat.desc", unknown_guid.c_str()));
	const auto known = call("desc.create", desc_args("materials/k.mat.desc",
		R"({"__type":"material_desc","shader_fragment":{"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6"}})"));
	const std::string unknown_with_path = R"({"__type":"material_desc","shader_fragment":{)" + guid + R"(,"path":"res://shaders/a.frag"}})";
	const auto by_path = call("desc.create", desc_args("materials/p.mat.desc", unknown_with_path.c_str()));
	rs->set_asset_index(nullptr);
	// A guid no file holds, with a path: the path is what loads, and the agent is told so.
	BOOST_REQUIRE_MESSAGE(by_path.ok, by_path.error);
	const json::array& warned = by_path.value.at("warnings").as_array();
	BOOST_REQUIRE_EQUAL(warned.size(), 1u);
	BOOST_TEST(std::string(warned[0].at("kind").as_string()) == std::string("reference"));
	BOOST_TEST(std::string(warned[0].at("json_path").as_string()) == std::string("/shader_fragment/guid"));
	for (const cmd::result* alone : { &unknown, &known }) {
		BOOST_TEST(alone->code == cmd::codes::invalid_args);
		BOOST_REQUIRE(alone->value.is_object());
		const json::value& error = alone->value.at("errors").as_array()[0];
		BOOST_TEST(std::string(error.at("json_path").as_string()) == std::string("/shader_fragment"));
		BOOST_TEST(std::string(error.at("reason").as_string()).find("a \"guid\" alone is not accepted here") != std::string::npos);
	}
	BOOST_TEST(!fs::exists(res_root / "materials" / "k.mat.desc"));
}

namespace {

// Path <-> ID both ways, as the editor's asset database answers.
struct ids_index final : res::asset_index
{
	std::map<std::string, res::asset_id, std::less<>> ids;
	std::map<res::asset_id, res::tag> paths;

	void hold(const res::asset_id& id, std::string_view path)
	{
		ids.emplace(std::string(path), id);
		paths.insert_or_assign(id, res::tag{ std::string(path) });
	}
	std::optional<res::asset_id> id_of(const res::tag& tag) const override
	{
		const auto found = ids.find(tag.view());
		return found == ids.end() ? std::nullopt : std::optional{ found->second };
	}
	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		const auto found = paths.find(id);
		return found == paths.end() ? std::nullopt : std::optional{ found->second };
	}
};

}

// #773: what desc.create and desc.write put on disk carries the ID of a target that has one, whatever form the agent sent;
// without an asset index the path stays a string. Writing the file again gives the same bytes.
BOOST_AUTO_TEST_CASE(AWrittenReference_CarriesTheIdOfItsTarget)
{
	const char* const content = R"({"__type":"material_desc","shader_fragment":"res://shaders/a.frag"})";
	make_material("materials/plain.mat.desc", content);
	BOOST_TEST(json::parse(read_file(res_root / "materials" / "plain.mat.desc")).at_pointer("/shader_fragment")
		== json::value("res://shaders/a.frag"));

	const res::asset_id id = *res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	auto index = std::make_shared<ids_index>();
	index->hold(id, "res://shaders/a.frag");
	rs->set_asset_index(index);
	make_material("materials/with_id.mat.desc", content);
	const std::string first = read_file(res_root / "materials" / "with_id.mat.desc");
	const auto rewritten = call("desc.write", write_args("materials/with_id.mat.desc", version_of("res://materials/with_id.mat.desc"), content));
	const std::string second = read_file(res_root / "materials" / "with_id.mat.desc");
	rs->set_asset_index(nullptr);

	const json::value expected = json::object{ { "guid", res::to_text(id) }, { "path", "res://shaders/a.frag" } };
	BOOST_TEST(json::parse(first).at_pointer("/shader_fragment") == expected);
	BOOST_REQUIRE_MESSAGE(rewritten.ok, rewritten.error);
	BOOST_TEST(second == first);
}

// #954: a guid the asset index holds must name the file the path names.
BOOST_AUTO_TEST_CASE(AParentWithAGuidOfTheSameFile_IsAccepted)
{
	auto index = std::make_shared<paths_index>();
	index->paths.emplace(*res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"), res::tag{ "res://base_material.desc" });
	rs->set_asset_index(index);
	const auto held = call("desc.create", desc_args("materials/g.mat.desc",
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://base_material.desc"}})"));
	const auto unknown = call("desc.create", desc_args("materials/u.mat.desc",
		R"({"__type":"material_desc","__parent":{"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6","path":"res://base_material.desc"}})"));
	rs->set_asset_index(nullptr);

	BOOST_CHECK_MESSAGE(held.ok, held.error);
	BOOST_CHECK_MESSAGE(unknown.ok, unknown.error);
	BOOST_TEST(fs::exists(res_root / "materials" / "g.mat.desc"));
	BOOST_TEST(fs::exists(res_root / "materials" / "u.mat.desc"));
}

BOOST_AUTO_TEST_CASE(AParentWithAGuidOfAnotherFile_IsRefused_AndNothingIsWritten)
{
	const char* content = R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://base_material.desc"}})";
	make_material("materials/w.mat.desc");
	const std::string before = read_file(res_root / "materials" / "w.mat.desc");
	const std::string version = version_of("materials/w.mat.desc");

	auto index = std::make_shared<paths_index>();
	index->paths.emplace(*res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"), res::tag{ "res://materials/other.mat.desc" });
	rs->set_asset_index(index);
	const auto created = call("desc.create", desc_args("materials/g.mat.desc", content));
	const auto written = call("desc.write", write_args("materials/w.mat.desc", version, content));
	rs->set_asset_index(nullptr);

	BOOST_TEST(created.code == cmd::codes::invalid_args);
	BOOST_TEST(created.error.find("name different files") != std::string::npos, created.error);
	BOOST_TEST(!fs::exists(res_root / "materials" / "g.mat.desc"));
	BOOST_TEST(written.code == cmd::codes::invalid_args);
	BOOST_TEST(read_file(res_root / "materials" / "w.mat.desc") == before);
}

BOOST_AUTO_TEST_CASE(AReferenceFieldWithAGuidOfAnotherFile_IsRefused_OfTheSameFile_IsWritten)
{
	const char* content = R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://shaders/a.frag"}})";
	make_material("materials/f.mat.desc");
	const std::string before = read_file(res_root / "materials" / "f.mat.desc");
	const std::string version = version_of("materials/f.mat.desc");
	const res::asset_id id = *res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");

	auto elsewhere = std::make_shared<paths_index>();
	elsewhere->paths.emplace(id, res::tag{ "res://shaders/b.frag" });
	rs->set_asset_index(elsewhere);
	const auto diverging = call("desc.write", write_args("materials/f.mat.desc", version, content));
	const std::string kept = read_file(res_root / "materials" / "f.mat.desc");
	auto same = std::make_shared<paths_index>();
	same->paths.emplace(id, res::tag{ "res://shaders/a.frag" });
	rs->set_asset_index(same);
	const auto consistent = call("desc.write", write_args("materials/f.mat.desc", version, content));
	rs->set_asset_index(nullptr);

	BOOST_TEST(diverging.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(diverging.value.is_object());
	const json::value& error = diverging.value.at("errors").as_array()[0];
	BOOST_TEST(std::string(error.at("json_path").as_string()) == std::string("/shader_fragment"));
	BOOST_TEST(std::string(error.at("reason").as_string()).find("name different files") != std::string::npos);
	BOOST_TEST(kept == before);
	BOOST_REQUIRE_MESSAGE(consistent.ok, consistent.error);
	BOOST_TEST(read_file(res_root / "materials" / "f.mat.desc").find("res://shaders/a.frag") != std::string::npos);
}

// A path that is no place a file may refer to is named as that, also when the guid is another file's.
BOOST_AUTO_TEST_CASE(ABadPath_IsReportedBeforeAGuidOfAnotherFile)
{
	auto index = std::make_shared<paths_index>();
	index->paths.emplace(*res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"), res::tag{ "res://shaders/b.frag" });
	rs->set_asset_index(index);
	const auto field = call("desc.create", desc_args("materials/o.mat.desc",
		R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"user://x.frag"}})"));
	const auto parent = call("desc.create", desc_args("materials/q.mat.desc",
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"user://secret.desc"}})"));
	rs->set_asset_index(nullptr);

	BOOST_TEST(field.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(field.value.is_object());
	const json::array& errors = field.value.at("errors").as_array();
	BOOST_REQUIRE(!errors.empty());
	BOOST_TEST(std::string(errors[0].at("json_path").as_string()) == std::string("/shader_fragment/path"));
	for (const json::value& error : errors) {
		BOOST_TEST(std::string(error.at("reason").as_string()).find("name different files") == std::string::npos);
	}
	BOOST_TEST(parent.code == cmd::codes::invalid_args);
	BOOST_TEST(parent.error.find("name different files") == std::string::npos, parent.error);
	BOOST_TEST(!fs::exists(res_root / "materials" / "o.mat.desc"));
	BOOST_TEST(!fs::exists(res_root / "materials" / "q.mat.desc"));
}

// A prefab's components have no schema here: a value shaped as a reference with a guid is held to the agent's rule.
BOOST_AUTO_TEST_CASE(AReferenceInAPrefabComponent_WithAGuidOfAnotherFile_IsRefused_OfTheSameFile_IsWritten)
{
	fs::create_directories(res_root / "prefabs");
	const char* const empty = R"({"__type":"prefab_desc","children":[{"__id":"00000000000000a1","name":"crate"}]})";
	const char* const content = R"({"__type":"prefab_desc","children":[{"__id":"00000000000000a1","name":"crate","components":)"
		R"({"material":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://materials/a.mat.desc"}}}]})";
	const auto made = call("desc.create", desc_args("prefabs/crate.desc", empty));
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	const std::string before = read_file(res_root / "prefabs" / "crate.desc");
	const std::string version = version_of("prefabs/crate.desc");
	const res::asset_id id = *res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");

	auto elsewhere = std::make_shared<paths_index>();
	elsewhere->paths.emplace(id, res::tag{ "res://materials/b.mat.desc" });
	rs->set_asset_index(elsewhere);
	const auto diverging = call("desc.write", write_args("prefabs/crate.desc", version, content));
	const std::string kept = read_file(res_root / "prefabs" / "crate.desc");
	auto same = std::make_shared<paths_index>();
	same->paths.emplace(id, res::tag{ "res://materials/a.mat.desc" });
	rs->set_asset_index(same);
	const auto consistent = call("desc.write", write_args("prefabs/crate.desc", version, content));
	rs->set_asset_index(nullptr);

	BOOST_TEST(diverging.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(diverging.value.is_object());
	const json::value& error = diverging.value.at("errors").as_array()[0];
	BOOST_TEST(std::string(error.at("json_path").as_string()) == std::string("/children/0/components/material"));
	BOOST_TEST(std::string(error.at("reason").as_string()).find("name different files") != std::string::npos);
	BOOST_TEST(kept == before);
	BOOST_REQUIRE_MESSAGE(consistent.ok, consistent.error);
	BOOST_TEST(read_file(res_root / "prefabs" / "crate.desc").find("res://materials/a.mat.desc") != std::string::npos);
}

// A "guid" that is no UUID is not a reference (the engine does not read it as one): the object is data and is kept as written.
BOOST_AUTO_TEST_CASE(AnObjectWithAGuidThatIsNoUuid_InAPrefabComponent_IsPlainData_AndIsWritten)
{
	fs::create_directories(res_root / "prefabs");
	const char* const empty = R"({"__type":"prefab_desc","children":[{"__id":"00000000000000a1","name":"crate"}]})";
	const char* const content = R"({"__type":"prefab_desc","children":[{"__id":"00000000000000a1","name":"crate","components":)"
		R"({"tag":{"guid":"abc"},"link":{"guid":"abc","path":"anything"}}}]})";
	const auto made = call("desc.create", desc_args("prefabs/crate.desc", empty));
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	const std::string version = version_of("prefabs/crate.desc");

	const auto written = call("desc.write", write_args("prefabs/crate.desc", version, content));

	BOOST_REQUIRE_MESSAGE(written.ok, written.error);
	// Looked for as values, wherever the writer puts them and however it lays the file out.
	const std::function<bool(const json::value&, const json::value&)> holds = [&holds](const json::value& in, const json::value& what) {
		if (in == what) return true;
		if (in.is_object()) {
			for (const auto& member : in.as_object()) {
				if (holds(member.value(), what)) return true;
			}
		}
		if (in.is_array()) {
			for (const json::value& item : in.as_array()) {
				if (holds(item, what)) return true;
			}
		}
		return false;
	};
	const json::value saved = json::parse(read_file(res_root / "prefabs" / "crate.desc"));
	BOOST_TEST(holds(saved, json::parse(R"({"guid":"abc"})")), json::serialize(saved));
	BOOST_TEST(holds(saved, json::parse(R"({"guid":"abc","path":"anything"})")), json::serialize(saved));

	const auto created = call("desc.create", desc_args("prefabs/crate2.desc", content));
	BOOST_TEST(created.ok, created.error);
}

// #968: while the editor's asset index is not complete a guid cannot be judged, so content that holds one is busy and
// nothing is written; content without a guid is written as before. The question is the ordinary one plus a line saying the
// write runs once the index is ready: an approved busy call is retried, so the person approves the write itself.
BOOST_AUTO_TEST_CASE(WhileTheAssetIndexIsPending_ContentWithAGuidIsBusy_AndNothingIsWritten)
{
	const char* const with_guid = R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://shaders/a.frag"}})";
	const char* const with_parent = R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://base_material.desc"}})";
	make_material("materials/w.mat.desc");
	const std::string before = read_file(res_root / "materials" / "w.mat.desc");
	const std::string version = version_of("materials/w.mat.desc");
	const std::optional<std::string> ready_question = commands.find("desc.write")->describe(call_of(write_args("materials/w.mat.desc", version, with_guid)));
	BOOST_REQUIRE(ready_question.has_value());
	BOOST_TEST(ready_question->rfind("Replace the desc file", 0) == 0u, *ready_question);
	index_pending = true;

	const auto written = call("desc.write", write_args("materials/w.mat.desc", version, with_guid));
	BOOST_TEST(written.code == cmd::codes::busy);
	BOOST_TEST(written.error.find("the asset index is still being built") != std::string::npos, written.error);
	BOOST_TEST(read_file(res_root / "materials" / "w.mat.desc") == before);
	const std::optional<std::string> question = commands.find("desc.write")->describe(call_of(write_args("materials/w.mat.desc", version, with_guid)));
	BOOST_REQUIRE(question.has_value());
	BOOST_TEST(question->rfind(*ready_question, 0) == 0u, *question);
	BOOST_TEST(question->substr(ready_question->size()).rfind("\nThe asset index is still being built: the file is replaced once it is ready", 0) == 0u, *question);

	const auto created = call("desc.create", desc_args("materials/g.mat.desc", with_guid));
	BOOST_TEST(created.code == cmd::codes::busy);
	BOOST_TEST(!fs::exists(res_root / "materials" / "g.mat.desc"));
	const auto parent = call("desc.create", desc_args("materials/p.mat.desc", with_parent));
	BOOST_TEST(parent.code == cmd::codes::busy);
	BOOST_TEST(!fs::exists(res_root / "materials" / "p.mat.desc"));

	// No guid: nothing waits for the index.
	const auto plain_create = call("desc.create", desc_args("materials/n.mat.desc", k_material));
	BOOST_TEST(plain_create.ok, plain_create.error);
	const auto plain_write = call("desc.write", write_args("materials/w.mat.desc", version, R"({"__type":"material_desc","queue":"opaque"})"));
	BOOST_TEST(plain_write.ok, plain_write.error);

	index_pending = false;
	const auto ready = call("desc.create", desc_args("materials/g.mat.desc", with_guid));
	BOOST_TEST(ready.ok, ready.error);
	BOOST_TEST(fs::exists(res_root / "materials" / "g.mat.desc"));
}

// The form of a reference does not depend on the index: it is refused as before, not held back.
BOOST_AUTO_TEST_CASE(WhileTheAssetIndexIsPending_AFormErrorIsStillRefused)
{
	index_pending = true;
	for (const char* content : {
		R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"}})",
		R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"user://x.frag"}})",
		R"({"__type":"material_desc","__parent":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"}})",
		R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://shaders/a.frag"},"bogus":1})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/f.mat.desc", content));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "f.mat.desc"));
		}
	}
}

// A guid that names another file than its path: while the index is pending the answer is busy, never a verdict made
// against an incomplete index; once it is ready the ordinary refusal comes.
BOOST_AUTO_TEST_CASE(AGuidOfAnotherFile_IsBusyWhileTheIndexIsPending_AndRefusedOnceItIsReady)
{
	const char* const content = R"({"__type":"material_desc","shader_fragment":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://shaders/a.frag"}})";
	make_material("materials/f.mat.desc");
	const std::string before = read_file(res_root / "materials" / "f.mat.desc");
	const std::string version = version_of("materials/f.mat.desc");
	auto elsewhere = std::make_shared<paths_index>();
	elsewhere->paths.emplace(*res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"), res::tag{ "res://shaders/b.frag" });
	rs->set_asset_index(elsewhere);

	index_pending = true;
	const auto held = call("desc.write", write_args("materials/f.mat.desc", version, content));
	const auto held_create = call("desc.create", desc_args("materials/g.mat.desc", content));
	// No verdict from the pending index: the person is asked about the write, which is refused once the index is ready.
	const std::optional<std::string> question = commands.find("desc.write")->describe(call_of(write_args("materials/f.mat.desc", version, content)));
	index_pending = false;
	const auto refused = call("desc.write", write_args("materials/f.mat.desc", version, content));
	rs->set_asset_index(nullptr);

	BOOST_TEST(held.code == cmd::codes::busy);
	BOOST_TEST(held.error.find("name different files") == std::string::npos, held.error);
	BOOST_TEST(held_create.code == cmd::codes::busy);
	BOOST_TEST(!fs::exists(res_root / "materials" / "g.mat.desc"));
	BOOST_REQUIRE(question.has_value());
	BOOST_TEST(question->rfind("Replace the desc file", 0) == 0u, *question);
	BOOST_TEST(question->find("The asset index is still being built") != std::string::npos, *question);
	BOOST_TEST(refused.code == cmd::codes::invalid_args);
	BOOST_TEST(refused.error.find("name different files") != std::string::npos, refused.error);
	BOOST_TEST(read_file(res_root / "materials" / "f.mat.desc") == before);
}

BOOST_AUTO_TEST_SUITE_END()
