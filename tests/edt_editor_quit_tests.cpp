#include <boost/test/unit_test.hpp>
#include "edt_editor_quit.h"
#include "edt_agent_confirmations.h"
#include "edt_mcp_bridge_host.h"
#include "test_manual_tasks.hpp"
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/editor/edt_document.h>
#include <snk/mcp/mcp_stdio.h>
#include <snk/resource/res_tag.h>
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <boost/json/object.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// editor.quit and the end of the stdio input (#452): who may stop a headless editor, and what happens to unsaved changes.
namespace {

namespace json = boost::json;

struct quit_fixture
{
	edt::editor_quit_sources sources()
	{
		edt::editor_quit_sources s;
		s.stdio_session = [this] { return session; };
		s.is_dirty = [this] { return dirty; };
		s.discard_changes = [this] {
			dirty = false;
			++discards;
		};
		s.request_stop = [this] { ++stops; };
		return s;
	}

	std::string session = "stdio-session";
	bool dirty = false;
	int discards = 0;
	int stops = 0;
};

class fake_document final : public edt::document
{
public:
	explicit fake_document(bool dirty) : m_dirty(dirty) {}

	std::string_view kind() const override { return "fake"; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return m_dirty; }
	std::uint64_t generation() const override { return 1; }

private:
	bool m_dirty;
	std::string m_title = "fake";
	res::tag m_source;
};

struct warning_capture
{
	warning_capture()
	{
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level != engine::log_level::warn) return;
			std::lock_guard lock(m);
			warnings.emplace_back(message);
		});
	}

	bool mentions(std::string_view text)
	{
		std::lock_guard lock(m);
		return std::ranges::any_of(warnings, [text](const std::string& w) { return w.find(text) != std::string::npos; });
	}

	std::mutex m;
	std::vector<std::string> warnings;
	engine::log_sink_handle sink;
};

}

BOOST_AUTO_TEST_SUITE(EdtEditorQuitTests)

BOOST_AUTO_TEST_CASE(OnlyAStdioHeadlessEditor_HasTheCommand)
{
	BOOST_TEST(edt::editor_quit_available(true, true));
	BOOST_TEST(!edt::editor_quit_available(true, false));
	BOOST_TEST(!edt::editor_quit_available(false, true));
	BOOST_TEST(!edt::editor_quit_available(false, false));
}

BOOST_FIXTURE_TEST_CASE(TheStdioSession_StopsTheEditor, quit_fixture)
{
	cmd::command_registry commands;
	const auto added = edt::register_editor_quit(commands, sources());
	BOOST_REQUIRE(added.active());
	const cmd::command_desc* desc = commands.find("editor.quit");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST((desc->category == cmd::agent_category::editor_control));
	BOOST_TEST(desc->mutates);
	BOOST_TEST(desc->expose_as_tool);
	BOOST_TEST((desc->risk == cmd::danger::none));

	const cmd::result result = commands.invoke("editor.quit", json::object{}, cmd::origin::agent("stdio-session"));
	BOOST_TEST(result.ok);
	BOOST_TEST(result.value.at("stopping").as_bool());
	BOOST_TEST(stops == 1);
	BOOST_TEST(discards == 0);
}

BOOST_FIXTURE_TEST_CASE(AnyoneElse_IsDenied, quit_fixture)
{
	cmd::command_registry commands;
	const auto added = edt::register_editor_quit(commands, sources());
	BOOST_TEST(commands.invoke("editor.quit", json::object{}, cmd::origin::agent("http-session")).code == cmd::codes::denied);
	BOOST_TEST(commands.invoke("editor.quit", json::object{}, cmd::origin::ui()).code == cmd::codes::denied);
	BOOST_TEST(commands.invoke("editor.quit", json::object{}, cmd::origin::test()).code == cmd::codes::denied);
	// No stdio client yet (or it went away): nobody matches.
	session.clear();
	BOOST_TEST(commands.invoke("editor.quit", json::object{}, cmd::origin::agent("")).code == cmd::codes::denied);
	BOOST_TEST(stops == 0);
}

BOOST_FIXTURE_TEST_CASE(UnsavedChanges_NeedDiscardUnsaved, quit_fixture)
{
	cmd::command_registry commands;
	const auto added = edt::register_editor_quit(commands, sources());
	dirty = true;
	BOOST_TEST(commands.invoke("editor.quit", json::object{}, cmd::origin::agent("stdio-session")).code == cmd::codes::needs_input);
	BOOST_TEST(commands.invoke("editor.quit", json::object{ { "discard_unsaved", false } }, cmd::origin::agent("stdio-session")).code
		== cmd::codes::needs_input);
	BOOST_TEST(stops == 0);
	BOOST_TEST(dirty);

	const cmd::result result = commands.invoke("editor.quit", json::object{ { "discard_unsaved", true } }, cmd::origin::agent("stdio-session"));
	BOOST_TEST(result.ok);
	BOOST_TEST(discards == 1);
	BOOST_TEST(!dirty);
	BOOST_TEST(stops == 1);
}

BOOST_FIXTURE_TEST_CASE(StdinClosedInHeadless_DropsUnsavedChangesWithAWarningAndStops, quit_fixture)
{
	warning_capture warnings;
	dirty = true;
	edt::on_stdio_input_closed(true, sources());
	BOOST_TEST(warnings.mentions("stdin closed with unsaved changes"));
	BOOST_TEST(discards == 1);
	BOOST_TEST(stops == 1);
}

BOOST_FIXTURE_TEST_CASE(StdinClosedInHeadless_WithoutChanges_StopsQuietly, quit_fixture)
{
	warning_capture warnings;
	edt::on_stdio_input_closed(true, sources());
	BOOST_TEST(!warnings.mentions("stdin closed"));
	BOOST_TEST(discards == 0);
	BOOST_TEST(stops == 1);
}

BOOST_FIXTURE_TEST_CASE(StdinClosedWithAWindow_KeepsTheEditorRunning, quit_fixture)
{
	dirty = true;
	edt::on_stdio_input_closed(false, sources());
	BOOST_TEST(discards == 0);
	BOOST_TEST(stops == 0);
	BOOST_TEST(dirty);
}

BOOST_AUTO_TEST_CASE(AnyUnsaved_LooksAtEveryOpenDocument)
{
	fake_document clean_scene(false);
	fake_document dirty_preview(true);
	const auto both = [&](const std::function<void(edt::document&)>& visit) {
		visit(clean_scene);
		visit(dirty_preview);
	};
	BOOST_TEST(edt::any_unsaved(both));
	const auto scene_only = [&](const std::function<void(edt::document&)>& visit) { visit(clean_scene); };
	BOOST_TEST(!edt::any_unsaved(scene_only));
	BOOST_TEST(!edt::any_unsaved(nullptr));
}

// The bridge behind both transports: an agent on HTTP cannot stop the editor, whatever it calls; the session ids are the
// real ones of a stdio transport and of the bridge's calls, not made up for the command.
BOOST_FIXTURE_TEST_CASE(AnHttpAgent_CannotQuit_ThroughTheBridge, quit_fixture)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	cmd::main_thread_queue queue{ commands, tasks };
	edt::session_grants grants;
	edt::agent_confirmations confirmations{ commands, grants };
	edt::mcp_bridge_host bridge{ commands, queue, tasks, confirmations };

	mcp::stdio_config config;
	config.write = [](std::string_view) { return true; };
	mcp::stdio_transport stdio(bridge, std::move(config));
	stdio.handle_line(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})");
	BOOST_REQUIRE(stdio.flush());
	session = stdio.session_id();
	BOOST_REQUIRE(!session.empty());
	edt::editor_quit_sources quit = sources();
	quit.stdio_session = [&stdio] { return stdio.session_id(); };
	const auto added = edt::register_editor_quit(commands, std::move(quit));
	BOOST_REQUIRE(added.active());

	// Over MCP: not even listed for an HTTP session (the bridge shows no editor_control command yet, #479).
	std::optional<mcp::call_result> over_mcp;
	mcp::request_context http;
	http.session_id = "0123456789abcdef0123456789abcdef";
	bridge.call_tool(http, "editor_quit", boost::json::object{}, nullptr, [&over_mcp](mcp::call_result r) { over_mcp = std::move(r); });
	tasks.drain_main(std::chrono::hours(1));
	BOOST_REQUIRE(over_mcp.has_value());
	BOOST_TEST(over_mcp->is_error);
	BOOST_TEST(stops == 0);

	// The path the bridge runs a tool on, with the HTTP session's origin: denied.
	auto refused = queue.call_now_or_post("editor.quit", json::object{}, cmd::origin::agent(http.session_id));
	BOOST_TEST(refused.get().code == cmd::codes::denied);
	BOOST_TEST(stops == 0);
	auto accepted = queue.call_now_or_post("editor.quit", json::object{}, cmd::origin::agent(stdio.session_id()));
	BOOST_TEST(accepted.get().ok);
	BOOST_TEST(stops == 1);
	stdio.stop();
}

BOOST_AUTO_TEST_SUITE_END()
