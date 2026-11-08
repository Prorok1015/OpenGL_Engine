#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_access.h"
#include "edt_agent_policy.h"
#include "edt_agent_server.h"
#include "edt_data_protocol.h"
#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include "edt_mcp_bridge_host.h"
#include <snk/application/app_deterministic_time.h>
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/command/cmd_registry.h>
#include <snk/config/cfg_variables.h>
#include <snk/gui/gui_null_backend.h>
#include <snk/gui/gui_platform_backend.h>
#include <snk/gui/gui_system.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_input_router.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #449: no command an agent can use writes the agent policy, the approved copy or the agent server's settings, and
// none approves. Runs a real headless editor_system over the null engine, with both the project and the user data
// folder in scratch directories, so the real home is never touched.
//
// Not covered, on purpose: commands only a window can run (the agents window itself: agent.settings.open is checked
// by id, its saves are the policy tests' job), commands registered after startup (a module reload adds some; the
// registry list is read once per test), and anything over the network (headless, no agent server transport runs, so
// the HTTP/stdio entry points are not exercised, only the registry gate they share).

namespace {

namespace fs = std::filesystem;

struct scratch_dirs
{
	test_util::temp_dir project{ "edt_agent_invariant_project" };
	test_util::temp_dir user{ "edt_agent_invariant_user" };
};

std::string read_bytes(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct snapshot
{
	std::map<std::string, std::string> files;
	std::string policy;
};

// What a command may create or change without breaking the invariant: diagnostics and caches, never a policy or a
// setting. Anything else that differs is a failure, so a new writer has to be named here on purpose.
constexpr std::string_view k_legit_writes[] = { "user/logs/", "project/.cache/" };

bool legitimately_written(const std::string& key)
{
	return std::any_of(std::begin(k_legit_writes), std::end(k_legit_writes), [&key](std::string_view prefix) { return key.starts_with(prefix); });
}

std::string changes_between(const snapshot& before, const snapshot& after)
{
	constexpr std::size_t k_shown = 20;
	std::vector<std::string> lines;
	for (const auto& [path, bytes] : after.files) {
		if (legitimately_written(path)) continue;
		const auto found = before.files.find(path);
		if (found == before.files.end()) {
			lines.push_back("appeared: " + path);
		} else if (found->second != bytes) {
			lines.push_back("changed: " + path);
		}
	}
	for (const auto& entry : before.files) {
		if (!legitimately_written(entry.first) && !after.files.contains(entry.first)) lines.push_back("removed: " + entry.first);
	}
	std::string out;
	for (std::size_t i = 0; i < lines.size() && i < k_shown; ++i) out += lines[i] + "\n";
	if (lines.size() > k_shown) out += "... and " + std::to_string(lines.size() - k_shown) + " more\n";
	if (before.policy != after.policy) out += "policy state:\n" + before.policy + "\n->\n" + after.policy + "\n";
	return out;
}

bool is_path_like(std::string_view name)
{
	return name == "path" || name == "paths" || name == "from" || name == "to" || name == "file" || name == "files"
		|| name == "tag" || name == "dir" || name == "folder";
}

const boost::json::object* properties_of(const cmd::command_desc& desc)
{
	if (!desc.args_schema.is_object()) return nullptr;
	const boost::json::value* properties = desc.args_schema.as_object().if_contains("properties");
	return properties != nullptr && properties->is_object() ? &properties->as_object() : nullptr;
}

bool has_schema_type(const boost::json::value& schema, std::string_view type)
{
	if (!schema.is_object()) return false;
	const boost::json::value* found = schema.as_object().if_contains("type");
	return found != nullptr && found->is_string() && std::string_view(found->as_string()) == type;
}

// The smallest value that passes the schema's type; only the name tells a code version from a text.
boost::json::value minimal_value(const boost::json::value& schema, std::string_view name)
{
	if (name == "version") return boost::json::string("0");
	if (schema.is_object()) {
		const boost::json::value* options = schema.as_object().if_contains("enum");
		if (options != nullptr && options->is_array() && !options->as_array().empty()) return options->as_array().front();
	}
	if (has_schema_type(schema, "boolean")) return false;
	if (has_schema_type(schema, "integer") || has_schema_type(schema, "number")) return 0;
	if (has_schema_type(schema, "array")) return boost::json::array{};
	if (has_schema_type(schema, "object")) return boost::json::object{};
	return boost::json::string("x");
}

boost::json::value path_value(const boost::json::value& schema, const std::string& path)
{
	if (has_schema_type(schema, "array")) return boost::json::array{ boost::json::string(path) };
	return boost::json::string(path);
}

// `target` empty: every path-like argument gets `value`; else only that one, the others a harmless file name.
boost::json::object hostile_args(const cmd::command_desc& desc, const boost::json::object& properties, const std::string& target,
	const std::string& value)
{
	const std::string other = "src/absent.cpp";
	boost::json::object args;
	for (const auto& entry : properties) {
		const std::string_view name = entry.key();
		if (is_path_like(name)) {
			args[name] = path_value(entry.value(), (target.empty() || name == target) ? value : other);
		} else if (name == "text" || name == "version") {
			args[name] = minimal_value(entry.value(), name);
		}
	}
	const boost::json::value* required = desc.args_schema.as_object().if_contains("required");
	if (required != nullptr && required->is_array()) {
		for (const boost::json::value& item : required->as_array()) {
			if (!item.is_string()) continue;
			const std::string_view name = item.as_string();
			if (args.contains(name)) continue;
			const boost::json::value* schema = properties.if_contains(name);
			args[name] = minimal_value(schema != nullptr ? *schema : boost::json::value{}, name);
		}
	}
	return args;
}

// A command that can run a process or touch git must stop at once in this fixture (no project, no toolchain).
bool must_fail_early(const cmd::command_desc& desc)
{
	using cmd::agent_category;
	if (desc.risk != cmd::danger::none) return true;
	if (!desc.mutates) return false;
	switch (desc.category) {
	case agent_category::build:
	case agent_category::live:
	case agent_category::vcs_commit:
	case agent_category::vcs_branch:
	case agent_category::vcs_sync:
	case agent_category::vcs_push:
		return true;
	default:
		return false;
	}
}

struct agent_editor_fixture : scratch_dirs, test_util::null_engine_fixture
{
	agent_editor_fixture()
	{
		cfg::set_variable("editor/headless", "true");
		// Without a project the policy file would be looked for in the working directory.
		cfg::set_variable("editor/project_dir", project.path().string());

		auto& rs = storage.require<res::resource_system>();
		rs.unregistrate_resolver(edt::DATA_PROTOCOL);
		edt::register_data_protocol(rs, SNK_EDITOR_DATA_DIR, user.path());

		storage.attach(std::shared_ptr<ds::task_system>(&tasks, [](ds::task_system*) {}));
		auto& commands = storage.require<cmd::command_registry>();
		auto& time = storage.construct<app::deterministic_time>();
		auto& control = storage.construct<app::frame_control>(time);
		storage.construct<app::frame_command_registrations>(app::register_frame_commands(commands, control));
		storage.construct<inp::input_router>(std::make_shared<inp::ecs_input_manager>());

		auto* render_driver = storage.require<rnd::render_system>().get_driver();
		auto window = storage.require<wnd::window_system>().get_active_window();
		BOOST_REQUIRE(render_driver);
		BOOST_REQUIRE(window);
		auto& backend = storage.construct<gui::imgui_backend_interface, gui::null_imgui_backend>(*render_driver);
		backend.init();
		auto& platform = storage.construct<gui::platform_backend>(*window);
		platform.init();
		storage.construct<gui::gui_system>(&backend, &platform);

		components::editor_init(storage);
		gui().run_without_drawing([this] { storage.require<edt::editor_system>().init(storage); });

		BOOST_REQUIRE(editor().agent_access_object());
		BOOST_REQUIRE(editor().agent_server_object());

		// code:// is mounted by the project loader, which this fixture does not run; without it every code.* command stops at
		// no_project and the path checks are never reached.
		project.write("code/src/game.cpp", "int game;\n");
		for (res::mount& mounted : res::resource_system::project_mount_table({}, { project.path() / "res" }, project.path() / ".cache",
				user.path() / "profile", code_dir())) {
			if (mounted.protocol == res::tag::code) rs.add_mount(std::move(mounted));
		}
		// A headless editor never approves; allowing it here makes "no command approves" a claim about the commands.
		access().policy().set_approval_allowed(true);
	}

	~agent_editor_fixture()
	{
		components::editor_term(storage);
		storage.destruct<gui::gui_system>();
		storage.require<gui::platform_backend>().shutdown();
		storage.destruct<gui::platform_backend>();
		storage.require<gui::imgui_backend_interface>().shutdown();
		storage.destruct<gui::imgui_backend_interface>();
		cfg::set_variable("editor/headless", "false");
		cfg::set_variable("editor/project_dir", "");
	}

	gui::gui_system& gui() { return storage.require<gui::gui_system>(); }
	edt::editor_system& editor() { return storage.require<edt::editor_system>(); }
	edt::agent_access& access() { return *editor().agent_access_object(); }
	cmd::command_registry& registry() { return storage.require<cmd::command_registry>(); }

	fs::path project_policy_file() const { return project.path() / ".snk" / "agent_policy.json"; }
	fs::path settings_dir() const { return user.path() / "settings"; }
	fs::path code_dir() const { return project.path() / "code"; }

	// What a person does in the agents window: every category allowed, saved and approved. `every_command` also gives each
	// registered command its own entry, which is the only thing that lifts a risk floor (build, deletes, git writes).
	void person_allows(bool every_command)
	{
		edt::policy_layer layer;
		for (const cmd::agent_category category : edt::settable_categories()) {
			layer.categories[category] = cmd::policy_mode::allow;
		}
		if (every_command) {
			for (const cmd::command_desc* desc : registry().list()) layer.commands[desc->id] = cmd::policy_mode::allow;
		}
		const auto error = access().policy().save_project(layer, edt::policy_layer{});
		BOOST_REQUIRE_MESSAGE(!error, error.value_or(""));
		BOOST_REQUIRE(!access().policy().project_pending_approval());
	}

	snapshot take_snapshot()
	{
		snapshot out;
		// Entries as themselves (#523): a link, dangling or not, is recorded with its target, never followed, so one an agent
		// planted at a policy path changes the snapshot.
		const auto collect = [&out](const fs::path& root, const std::string& label) {
			std::error_code ec;
			if (fs::symlink_status(root, ec).type() == fs::file_type::not_found) return;
			fs::recursive_directory_iterator it(root, ec);
			BOOST_TEST_REQUIRE(!ec, "cannot list " << root.string() << ": " << ec.message());
			for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
				BOOST_TEST_REQUIRE(!ec, "cannot list " << root.string() << ": " << ec.message());
				const fs::path& path = it->path();
				const std::string key = label + "/" + path.lexically_relative(root).generic_string();
				const fs::file_status status = it->symlink_status(ec);
				BOOST_TEST_REQUIRE(!ec, "cannot examine " << path.string() << ": " << ec.message());
				bool link = fs::is_symlink(status);
#ifdef _MSC_VER
				link = link || status.type() == fs::file_type::junction;
#endif
				if (link) {
					const fs::path target = fs::read_symlink(path, ec);
					BOOST_TEST_REQUIRE(!ec, "cannot read the link " << path.string() << ": " << ec.message());
					out.files[key] = "<link> " + target.generic_string();
				} else if (fs::is_directory(status)) {
					out.files[key + "/"] = "<dir>";
				} else if (fs::is_regular_file(status)) {
					out.files[key] = read_bytes(path);
				} else {
					out.files[key] = "<other>";
				}
			}
			BOOST_TEST_REQUIRE(!ec, "cannot list " << root.string() << ": " << ec.message());
		};
		collect(project.path(), "project");
		collect(user.path(), "user");

		edt::agent_policy& policy = access().policy();
		out.policy = edt::serialize_policy_layer(policy.project()) + "\n--\n" + edt::serialize_policy_layer(policy.personal())
			+ "\n--\n" + edt::serialize_policy_layer(policy.approved_project())
			+ "\npending=" + (policy.project_pending_approval() ? "1" : "0");
		const edt::agent_server& server = *editor().agent_server_object();
		out.policy += "\nserver active=" + std::string(server.active() ? "1" : "0") + " running=" + (server.running() ? "1" : "0")
			+ " port=" + std::to_string(server.port());
		return out;
	}

	std::vector<std::string> agent_visible_ids()
	{
		std::vector<std::string> ids;
		for (const cmd::command_desc* desc : registry().list()) {
			if (edt::agent_may_use(*desc, access().policy())) ids.push_back(desc->id);
		}
		return ids;
	}

	cmd::result invoke_as_agent(const std::string& id, const boost::json::value& args)
	{
		cmd::call request;
		request.args = args;
		request.from = cmd::origin::agent("agent-invariant-test");
		cmd::result outcome;
		gui().run_without_drawing([&] {
			outcome = registry().invoke(id, request);
			editor().run_implicit_callbacks();
		});
		test_util::pump(tasks);
		return outcome;
	}

	// Ids, not descriptors: a command may register or remove others while it runs.
	std::size_t call_every_agent_command()
	{
		std::size_t called = 0;
		for (const std::string& id : agent_visible_ids()) {
			const cmd::result outcome = invoke_as_agent(id, boost::json::object{});
			const cmd::command_desc* desc = registry().find(id);
			if (desc != nullptr && must_fail_early(*desc)) {
				BOOST_TEST_CONTEXT(id << " answered " << outcome.code) {
					BOOST_TEST(!outcome.ok);
				}
			}
			++called;
		}
		return called;
	}

	std::vector<std::string> hostile_paths()
	{
		const fs::path settings = settings_dir();
		std::vector<std::string> out{ "../.snk/agent_policy.json", ".snk/agent_policy.json", ".snk", "..", "../escape.cpp",
			"agent_policy.json", "agent_server.json",
			(settings / "agent_policy.json").lexically_relative(code_dir()).generic_string(),
			(settings / "agent_server.json").lexically_relative(code_dir()).generic_string() };
		const auto add_absolute = [&out](const fs::path& file) {
			out.push_back(file.generic_string());
			out.push_back(file.string());
		};
		add_absolute(project_policy_file());
		add_absolute(access().policy().personal_file());
		add_absolute(settings / "agent_server.json");
		add_absolute(edt::approved_snapshot_for(settings, project.path()).file);
		std::error_code ec;
		add_absolute(edt::approved_snapshot_for(settings, fs::weakly_canonical(project.path(), ec)).file);
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	// Every command with a path-like argument (found by its schema) gets every hostile value, in all such arguments at once and
	// in each alone. Returns the number of calls. The code.* commands must refuse every one of them.
	std::size_t call_every_path_command_with_hostile_paths()
	{
		std::vector<std::string> ids;
		for (const cmd::command_desc* desc : registry().list()) ids.push_back(desc->id);
		const std::vector<std::string> hostile = hostile_paths();
		std::size_t calls = 0;
		for (const std::string& id : ids) {
			const cmd::command_desc* desc = registry().find(id);
			const boost::json::object* properties = desc != nullptr ? properties_of(*desc) : nullptr;
			if (properties == nullptr) continue;
			std::vector<std::string> targets;
			for (const auto& entry : *properties) {
				if (is_path_like(entry.key())) targets.emplace_back(entry.key());
			}
			if (targets.empty()) continue;
			std::vector<std::string> variants{ std::string{} };
			if (targets.size() > 1) variants.insert(variants.end(), targets.begin(), targets.end());
			for (const std::string& value : hostile) {
				for (const std::string& target : variants) {
					const boost::json::object args = hostile_args(*desc, *properties, target, value);
					const cmd::result outcome = invoke_as_agent(id, args);
					++calls;
					if (id.starts_with("code.")) {
						BOOST_TEST_CONTEXT(id << " " << (target.empty() ? "<all>" : target) << " = " << value << " answered " << outcome.code) {
							BOOST_TEST(!outcome.ok);
						}
					}
				}
			}
		}
		return calls;
	}

	void expect_agent_files_absent()
	{
		// symlink_status: a dangling link at one of these names is something an agent left behind too.
		const auto absent = [](const fs::path& path) {
			std::error_code ec;
			return fs::symlink_status(path, ec).type() == fs::file_type::not_found;
		};
		BOOST_TEST(absent(project_policy_file()));
		BOOST_TEST(absent(settings_dir() / "agent_policy.json"));
		BOOST_TEST(absent(settings_dir() / "agent_server.json"));
		BOOST_TEST(absent(settings_dir() / "agent_policy_approved"));
	}
};

bool names_agent_policy(const std::string& id)
{
	const auto has = [&id](const char* part) { return id.find(part) != std::string::npos; };
	return has("policy") || has("approv") || has("agent_server") || has("agent.server") || id.starts_with("agent.settings");
}

}

BOOST_AUTO_TEST_SUITE(EdtAgentInvariant)

BOOST_FIXTURE_TEST_CASE(NoAgentCommand_WritesThePolicyOrTheServerSettings_UnderTheDefaultPolicy, agent_editor_fixture)
{
	expect_agent_files_absent();
	const snapshot before = take_snapshot();

	BOOST_TEST(call_every_agent_command() > 0u);

	BOOST_TEST(changes_between(before, take_snapshot()) == std::string{});
	expect_agent_files_absent();
}

BOOST_FIXTURE_TEST_CASE(NoAgentCommand_ChangesThePolicyAPersonSaved_EvenWithEveryCategoryAllowed, agent_editor_fixture)
{
	person_allows(false);
	const snapshot before = take_snapshot();
	BOOST_REQUIRE(before.files.contains("project/.snk/agent_policy.json"));

	BOOST_TEST(call_every_agent_command() > 0u);

	BOOST_TEST(changes_between(before, take_snapshot()) == std::string{});
}

BOOST_FIXTURE_TEST_CASE(NoAgentCommand_ChangesAnyFile_WhenEveryCommandIsAllowedByName, agent_editor_fixture)
{
	person_allows(false);
	const std::size_t visible_by_category = agent_visible_ids().size();
	person_allows(true);
	const std::vector<std::string> visible = agent_visible_ids();
	// The risk floors are lifted: what a category entry never opened (builds, deletes, git writes) is on the list and gets called.
	BOOST_TEST(visible.size() > visible_by_category);
	for (const char* id : { "code.write", "code.create", "code.rename", "code.delete", "code.read", "build.game_module", "vcs.commit", "vcs.push" }) {
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST((std::find(visible.begin(), visible.end(), id) != visible.end()));
		}
	}
	// Past the gate (not denied, not asked): the arguments are what stops it.
	BOOST_TEST(invoke_as_agent("code.write", boost::json::object{}).code == cmd::codes::invalid_args);

	const snapshot before = take_snapshot();
	BOOST_TEST(call_every_agent_command() > 0u);
	BOOST_TEST(changes_between(before, take_snapshot()) == std::string{});
}

BOOST_FIXTURE_TEST_CASE(HostilePaths_ChangeNoFile_UnderTheDefaultPolicy, agent_editor_fixture)
{
	const snapshot before = take_snapshot();

	BOOST_TEST(call_every_path_command_with_hostile_paths() > 0u);

	BOOST_TEST(changes_between(before, take_snapshot()) == std::string{});
	expect_agent_files_absent();
}

BOOST_FIXTURE_TEST_CASE(HostilePaths_ChangeNoFile_WhenEveryCommandIsAllowedByName, agent_editor_fixture)
{
	person_allows(true);
	const snapshot before = take_snapshot();
	BOOST_REQUIRE(before.files.contains("project/.snk/agent_policy.json"));

	BOOST_TEST(call_every_path_command_with_hostile_paths() > 0u);

	BOOST_TEST(changes_between(before, take_snapshot()) == std::string{});
}

BOOST_FIXTURE_TEST_CASE(CommandsThatNameThePolicy_AreNotForAgentsOrReadOnly, agent_editor_fixture)
{
	const cmd::command_desc* settings = registry().find("agent.settings.open");
	BOOST_REQUIRE(settings != nullptr);
	BOOST_TEST((settings->category == cmd::agent_category::not_for_agents));
	BOOST_TEST(!edt::agent_may_use(*settings, access().policy()));

	for (const cmd::command_desc* desc : registry().list()) {
		if (!names_agent_policy(desc->id)) continue;
		BOOST_TEST_CONTEXT(desc->id) {
			BOOST_TEST((desc->category == cmd::agent_category::not_for_agents || !desc->mutates));
		}
	}

	person_allows(true);
	BOOST_TEST(!edt::agent_may_use(*settings, access().policy()));
	for (const std::string& id : agent_visible_ids()) {
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST(id.find("approv") == std::string::npos);
			BOOST_TEST(id.find("save_policy") == std::string::npos);
		}
	}

	const cmd::result outcome = registry().invoke("agent.settings.open", boost::json::object{}, cmd::origin::agent("agent-invariant-test"));
	BOOST_TEST(!outcome.ok);
}

BOOST_FIXTURE_TEST_CASE(OpeningTheAgentsWindow_WritesNoPolicyFile, agent_editor_fixture)
{
	const snapshot before = take_snapshot();

	gui().run_without_drawing([&] {
		registry().invoke("agent.settings.open", boost::json::object{}, cmd::origin::ui());
		editor().run_implicit_callbacks();
	});
	test_util::pump(tasks);

	BOOST_TEST(changes_between(before, take_snapshot()) == std::string{});
	expect_agent_files_absent();
}

BOOST_AUTO_TEST_SUITE_END()

// The real begin_level_load / level_tag path of the editor (#547): the document belongs to a level file only once that level
// is loaded, and nothing writes it in between.
BOOST_AUTO_TEST_SUITE(EdtAgentLevelFiles)

namespace {

std::vector<std::byte> bytes_of_resource(res::resource_system& resources, const char* relative)
{
	auto raw = resources.fetch_data(res::tag::make(relative));
	BOOST_REQUIRE(raw);
	raw->wait();
	BOOST_REQUIRE(raw->is_ready());
	return raw->data;
}

}

BOOST_FIXTURE_TEST_CASE(AnAgentCannotSaveWhileALevelLoads_AndTheOldLevelFileIsKept, agent_editor_fixture)
{
	const auto pump_level_load = [this] {
		for (int i = 0; i < 400 && editor().level_load_in_progress(); ++i) {
			gui().run_without_drawing([this] { editor().run_implicit_callbacks(); });
			test_util::pump(tasks);
		}
		BOOST_REQUIRE(!editor().level_load_in_progress());
	};

	BOOST_REQUIRE(invoke_as_agent("file.open", boost::json::object{ { "path", "levels/smoke_level.desc" } }).ok);
	pump_level_load();
	BOOST_REQUIRE_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");

	// The open starts the load; the frame that would finish it has not run yet.
	cmd::call open;
	open.args = boost::json::object{ { "path", "levels/test_level.desc" } };
	open.from = cmd::origin::agent("agent-invariant-test");
	cmd::result started;
	gui().run_without_drawing([&] { started = registry().invoke("file.open", open); });
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	BOOST_REQUIRE(editor().level_load_in_progress());

	// The document still belongs to the old level, and nobody writes it now: not the agent, not a person.
	BOOST_CHECK_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");
	const std::vector<std::byte> next_before = bytes_of_resource(storage.require<res::resource_system>(), "levels/test_level.desc");
	for (const cmd::origin& from : { cmd::origin::agent("agent-invariant-test"), cmd::origin::ui() }) {
		cmd::call save;
		save.from = from;
		BOOST_CHECK_EQUAL(registry().invoke("file.save", save).code, "busy");
		save.args = boost::json::object{ { "path", "levels/copy.desc" } };
		BOOST_CHECK_EQUAL(registry().invoke("file.save_as", save).code, "busy");
		save.args = boost::json::object{ { "path", "levels/other.desc" } };
		BOOST_CHECK_EQUAL(registry().invoke("file.open", save).code, "busy");
	}
	BOOST_CHECK(bytes_of_resource(storage.require<res::resource_system>(), "levels/test_level.desc") == next_before);

	pump_level_load();
	BOOST_CHECK_EQUAL(std::string(editor().current_level_tag().relative()), "levels/test_level.desc");
	BOOST_CHECK(bytes_of_resource(storage.require<res::resource_system>(), "levels/test_level.desc") == next_before);
}

// The working copy of a load is its own: a second level with the same file name from another folder, abandoned, leaves the
// copy the document's level is read from as it was.
BOOST_FIXTURE_TEST_CASE(AnAbandonedLoadOfASameNamedLevel_LeavesTheEditedLevelsCopyAlone, agent_editor_fixture)
{
	const auto pump_level_load = [this] {
		for (int i = 0; i < 400 && editor().level_load_in_progress(); ++i) {
			gui().run_without_drawing([this] { editor().run_implicit_callbacks(); });
			test_util::pump(tasks);
		}
		BOOST_REQUIRE(!editor().level_load_in_progress());
	};
	auto& resources = storage.require<res::resource_system>();

	BOOST_REQUIRE(invoke_as_agent("file.open", boost::json::object{ { "path", "levels/smoke_level.desc" } }).ok);
	pump_level_load();
	BOOST_REQUIRE_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");
	const res::tag copy = editor().current_editor_tag();
	BOOST_REQUIRE(copy.is_valid());
	const std::string copy_view(copy.view());
	const auto copy_bytes = [&] {
		auto raw = resources.fetch_data(res::tag{ copy_view });
		BOOST_REQUIRE(raw);
		raw->wait();
		BOOST_REQUIRE(raw->is_ready());
		return raw->data;
	};
	const std::vector<std::byte> copy_before = copy_bytes();
	BOOST_REQUIRE(!copy_before.empty());

	// Same stem, another folder, and no level: the load fails.
	const std::string junk = "not a level";
	std::vector<std::byte> junk_bytes(junk.size());
	std::transform(junk.begin(), junk.end(), junk_bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
	BOOST_REQUIRE(resources.store_sync(res::tag::make("other/smoke_level.desc"), junk_bytes));
	BOOST_CHECK(invoke_as_agent("file.open", boost::json::object{ { "path", "other/smoke_level.desc" } }).ok);
	pump_level_load();

	BOOST_CHECK_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");
	BOOST_CHECK_EQUAL(std::string(editor().current_editor_tag().view()), copy_view);
	BOOST_CHECK(copy_bytes() == copy_before);
}

// Opening levels in a loop (an agent can) leaves one working copy in memory://: the document's own. A failed load takes its
// copy with it, a successful one takes the previous level's.
BOOST_FIXTURE_TEST_CASE(ManyOpens_LeaveOnlyTheCurrentLevelsWorkingCopy, agent_editor_fixture)
{
	const auto pump_level_load = [this] {
		for (int i = 0; i < 400 && editor().level_load_in_progress(); ++i) {
			gui().run_without_drawing([this] { editor().run_implicit_callbacks(); });
			test_util::pump(tasks);
		}
		BOOST_REQUIRE(!editor().level_load_in_progress());
	};
	auto& resources = storage.require<res::resource_system>();
	const auto exists = [&resources](const std::string& tag) {
		auto raw = resources.fetch_data(res::tag{ tag });
		if (!raw) return false;
		raw->wait();
		return raw->is_ready();
	};
	const std::string junk = "not a level";
	std::vector<std::byte> junk_bytes(junk.size());
	std::transform(junk.begin(), junk.end(), junk_bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
	BOOST_REQUIRE(resources.store_sync(res::tag::make("other/smoke_level.desc"), junk_bytes));
	BOOST_REQUIRE(resources.store_sync(res::tag::make("other/test_level.desc"), junk_bytes));

	// The working copies of the loads in the order they begin: 1..6.
	const std::vector<std::pair<std::string, bool>> opens{
		{ "levels/smoke_level.desc", true }, { "other/smoke_level.desc", false }, { "levels/test_level.desc", true },
		{ "other/test_level.desc", false }, { "levels/smoke_level.desc", true }, { "other/smoke_level.desc", false } };
	std::vector<std::string> copies;
	std::string current_copy;
	for (const auto& [path, is_level] : opens) {
		const std::string stem = fs::path(path).stem().string();
		const std::string copy = "memory://editor/loading/" + std::to_string(copies.size() + 1) + "/" + stem + ".desc";
		copies.push_back(copy);
		BOOST_TEST_CONTEXT("open " << path) {
			BOOST_REQUIRE(invoke_as_agent("file.open", boost::json::object{ { "path", path } }).ok);
			pump_level_load();
			if (is_level) current_copy = copy;
			BOOST_CHECK_EQUAL(std::string(editor().current_editor_tag().view()), current_copy);
		}
	}
	for (const std::string& copy : copies) {
		BOOST_TEST_CONTEXT(copy) { BOOST_CHECK_EQUAL(exists(copy), copy == current_copy); }
	}
	BOOST_CHECK_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");
}

BOOST_FIXTURE_TEST_CASE(AnAgentOpeningAFileThatIsNoLevel_LeavesTheDocumentOnItsOldLevel, agent_editor_fixture)
{
	const auto pump_level_load = [this] {
		for (int i = 0; i < 400 && editor().level_load_in_progress(); ++i) {
			gui().run_without_drawing([this] { editor().run_implicit_callbacks(); });
			test_util::pump(tasks);
		}
		BOOST_REQUIRE(!editor().level_load_in_progress());
	};
	auto& resources = storage.require<res::resource_system>();

	BOOST_REQUIRE(invoke_as_agent("file.open", boost::json::object{ { "path", "levels/smoke_level.desc" } }).ok);
	pump_level_load();
	BOOST_REQUIRE_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");
	const std::vector<std::byte> material_before = bytes_of_resource(resources, "base_material.desc");

	// A material is a .desc but not a level: the open is refused before anything loads (reading it as a level asserts in the
	// desc adapter), and the document keeps its level.
	const cmd::result refused = invoke_as_agent("file.open", boost::json::object{ { "path", "base_material.desc" } });
	BOOST_CHECK(!refused.ok);
	BOOST_CHECK_EQUAL(refused.code, "invalid_args");
	BOOST_CHECK(!editor().level_load_in_progress());
	pump_level_load();
	BOOST_CHECK_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");

	// The failed open did not make the material the file to save to: the rule "another existing file needs input" holds.
	const cmd::result over = invoke_as_agent("file.save_as", boost::json::object{ { "path", "base_material.desc" } });
	BOOST_CHECK(!over.ok);
	BOOST_CHECK_EQUAL(over.code, "needs_input");
	BOOST_CHECK(bytes_of_resource(resources, "base_material.desc") == material_before);

	// A plain save goes to the old level, not to the material.
	const cmd::result saved = invoke_as_agent("file.save", boost::json::object{});
	BOOST_CHECK_MESSAGE(saved.ok, saved.error);
	BOOST_CHECK(bytes_of_resource(resources, "base_material.desc") == material_before);
	BOOST_CHECK_EQUAL(std::string(editor().current_level_tag().relative()), "levels/smoke_level.desc");
}

BOOST_AUTO_TEST_SUITE_END()

