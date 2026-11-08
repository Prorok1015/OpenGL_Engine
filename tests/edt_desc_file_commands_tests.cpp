#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include <snk/command/cmd_policy.h>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
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

const char* const k_material = R"({"__type":"material_desc","queue":"mix","defines":["A"]})";

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
		edt::desc_file_deps deps{ *rs, *descs };
		deps.top_layer = [this] { return res_root; };
		deps.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		deps.open_level_files = [this] { return open_level; };
		deps.events = &events;
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

BOOST_AUTO_TEST_CASE(ATagThatLeavesTheProject_IsRefused_InParentAndInEveryTagField)
{
	for (const char* content : {
		R"({"__type":"material_desc","__parent":"user://secret.desc"})",
		R"({"__type":"material_desc","__parent":"edt://base.desc"})",
		R"({"__type":"material_desc","__parent":"../x.desc"})",
		R"({"__type":"material_desc","__parent":"res://a/../../x.desc"})",
		R"({"__type":"material_desc","__parent":"res://a/x.png"})",
		R"({"__type":"material_desc","__parent":5})",
		R"({"__type":"material_desc","shader_fragment":"user://x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"engine://x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"cache://x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"../../x.frag"})",
		R"({"__type":"material_desc","shader_fragment":"C:/x.frag"})",
		R"({"__type":"material_desc","defines":["code://x"]})",
		R"({"__type":"material_desc","samplers":["../../x.png"]})",
		R"({"__type":"material_desc","samplers":["user://x.png"]})",
		R"({"__type":"level_desc","worlds":["memory://x"]})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto refused = call("desc.create", desc_args("materials/t.mat.desc", content));
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
			BOOST_TEST(!fs::exists(res_root / "materials" / "t.mat.desc"));
		}
	}
	for (const char* content : {
		R"({"__type":"material_desc","__parent":"res://base_material.desc"})",
		R"({"__type":"material_desc","__parent":"materials/other.mat.desc"})",
		R"({"__type":"material_desc","shader_fragment":"res://shaders/a.frag"})",
		R"({"__type":"material_desc","shader_fragment":"shaders/a.frag"})",
		R"({"__type":"material_desc","samplers":["res://textures/a.png",null]})" }) {
		BOOST_TEST_CONTEXT(content) {
			const auto made = call("desc.create", desc_args("materials/ok.mat.desc", content));
			BOOST_TEST_MESSAGE(made.error);
			BOOST_TEST(made.ok);
			fs::remove(res_root / "materials" / "ok.mat.desc");
		}
	}
}

BOOST_AUTO_TEST_CASE(AnUnknownKeyWithTwoUnderscores_IsRefused_SoNoObjectHidesFromTheCheck)
{
	const auto hidden = call("desc.create", desc_args("materials/h.mat.desc", R"({"__type":"material_desc","__x":{"__type":"no_such_desc"}})"));
	BOOST_TEST(hidden.code == cmd::codes::invalid_args);
	BOOST_REQUIRE(hidden.value.is_object());
	BOOST_TEST(std::string(hidden.value.at("errors").as_array()[0].at("json_path").as_string()) == std::string("/__x"));
	BOOST_TEST(!fs::exists(res_root / "materials" / "h.mat.desc"));

	const auto kept = call("desc.create", desc_args("materials/k.mat.desc", R"({"__type":"material_desc","__guid":"g","__id":3})"));
	BOOST_TEST_MESSAGE(kept.error);
	BOOST_TEST(kept.ok);
}

BOOST_AUTO_TEST_CASE(ManyProblems_AreCappedWithoutLettingTheFilePass)
{
	std::string worlds;
	for (int i = 0; i < 30; ++i) worlds += std::string(i ? "," : "") + R"({"__type":"material_desc","bogus":1})";
	const std::string content = R"({"__type":"level_desc","worlds":[)" + worlds + "]}";
	const std::vector<res::diagnostic> found = edt::validate_desc_tree(*descs, json::parse(content), "res://levels/x.desc");
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

BOOST_AUTO_TEST_SUITE_END()
