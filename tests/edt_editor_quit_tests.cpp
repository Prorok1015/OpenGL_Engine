#include <boost/test/unit_test.hpp>
#include "edt_editor_quit.h"
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <boost/json/object.hpp>
#include <algorithm>
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

BOOST_AUTO_TEST_SUITE_END()
