#include <boost/test/unit_test.hpp>
#include <snk/project/prj_modules.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

prj::manifest manifest_with(std::vector<prj::module_ref> modules)
{
	prj::manifest m;
	m.name = "Sandbox";
	m.modules = std::move(modules);
	return m;
}

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

	bool mentions(std::string_view name)
	{
		std::lock_guard lock(m);
		return std::ranges::any_of(warnings, [name](const std::string& w) { return w.find(name) != std::string::npos; });
	}

	std::mutex m;
	std::vector<std::string> warnings;
	engine::log_sink_handle sink;
};

}

BOOST_AUTO_TEST_SUITE(PrjModulesTests)

BOOST_AUTO_TEST_CASE(Resolve_SortsManifestModulesByWhatTheHostKnows)
{
	const auto result = prj::resolve_modules(manifest_with({
		{ "game", prj::module_kind::runtime, false },
		{ "physics", prj::module_kind::runtime, false },
		{ "extras", prj::module_kind::runtime, true },
		{ "game_editor", prj::module_kind::editor, false },
	}), { "game", "game_editor" }, true);

	BOOST_REQUIRE(result.resolved.size() == 2u);
	BOOST_TEST(result.resolved[0].name == "game");
	BOOST_TEST(result.resolved[1].name == "game_editor");
	BOOST_TEST(result.missing_required == std::vector<std::string>{ "physics" });
	BOOST_TEST(result.missing_optional == std::vector<std::string>{ "extras" });
}

BOOST_AUTO_TEST_CASE(Resolve_SkipsEditorModulesWhenAskedTo)
{
	const auto result = prj::resolve_modules(manifest_with({
		{ "game_editor", prj::module_kind::editor, false },
		{ "tools", prj::module_kind::editor, false },
	}), { "game_editor" }, false);

	BOOST_TEST(result.resolved.empty());
	BOOST_TEST(result.missing_required.empty());
	BOOST_TEST(result.missing_optional.empty());
}

BOOST_AUTO_TEST_CASE(Resolve_WarnsForAMissingRequiredModuleOnly)
{
	warning_capture capture;
	prj::resolve_modules(manifest_with({
		{ "needed", prj::module_kind::runtime, false },
		{ "nice_to_have", prj::module_kind::runtime, true },
	}), {}, true);

	BOOST_TEST(capture.mentions("'needed'"));
	BOOST_TEST(!capture.mentions("'nice_to_have'"));
}

BOOST_AUTO_TEST_SUITE_END()
