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
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

// #449: no command an agent can use writes the agent policy, the approved copy or the agent server's settings, and
// none approves. Runs a real headless editor_system over the null engine, with both the project and the user data
// folder in scratch directories, so the real home is never touched.

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

std::string changes_between(const snapshot& before, const snapshot& after)
{
	std::string out;
	for (const auto& [path, bytes] : after.files) {
		const auto found = before.files.find(path);
		if (found == before.files.end()) {
			out += "appeared: " + path + "\n";
		} else if (found->second != bytes) {
			out += "changed: " + path + "\n";
		}
	}
	for (const auto& entry : before.files) {
		if (!after.files.contains(entry.first)) out += "removed: " + entry.first + "\n";
	}
	if (before.policy != after.policy) out += "policy state:\n" + before.policy + "\n->\n" + after.policy + "\n";
	return out;
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

	// What a person does in the agents window: every category allowed, saved and approved.
	void person_allows_every_category()
	{
		edt::policy_layer layer;
		for (const cmd::agent_category category : edt::settable_categories()) {
			layer.categories[category] = cmd::policy_mode::allow;
		}
		const auto error = access().policy().save_project(layer);
		BOOST_REQUIRE_MESSAGE(!error, error.value_or(""));
		BOOST_REQUIRE(!access().policy().project_pending_approval());
	}

	snapshot take_snapshot()
	{
		snapshot out;
		const auto collect = [&out](const fs::path& root, const std::string& label) {
			std::error_code ec;
			if (!fs::exists(root, ec)) return;
			for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
				if (!entry.is_regular_file()) continue;
				const std::string rel = fs::relative(entry.path(), root).generic_string();
				if (rel.find("agent_") == std::string::npos) continue;
				out.files[label + "/" + rel] = read_bytes(entry.path());
			}
		};
		collect(project.path() / ".snk", "project");
		collect(settings_dir(), "settings");

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

	// Ids, not descriptors: a command may register or remove others while it runs.
	std::size_t call_every_agent_command()
	{
		std::size_t called = 0;
		for (const std::string& id : agent_visible_ids()) {
			cmd::call request;
			request.from = cmd::origin::agent("agent-invariant-test");
			gui().run_without_drawing([&] {
				registry().invoke(id, request);
				editor().run_implicit_callbacks();
			});
			test_util::pump(tasks);
			++called;
		}
		return called;
	}

	void expect_agent_files_absent()
	{
		BOOST_TEST(!fs::exists(project_policy_file()));
		BOOST_TEST(!fs::exists(settings_dir() / "agent_policy.json"));
		BOOST_TEST(!fs::exists(settings_dir() / "agent_server.json"));
		BOOST_TEST(!fs::exists(settings_dir() / "agent_policy_approved"));
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
	person_allows_every_category();
	const snapshot before = take_snapshot();
	BOOST_REQUIRE(before.files.contains("project/agent_policy.json"));

	BOOST_TEST(call_every_agent_command() > 0u);

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

	person_allows_every_category();
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
