#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/resource/resources/res_resource_text.h>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// resource_system::warn_on_sync_require (#925): a require_sync of a tag whose adapter can block the main thread for
// seconds (the editor's models, parsed in the decoder process) warns once per tag.

namespace {

struct warnings
{
	warnings()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) lines.push_back(std::format("{}: {}", category, message));
		});
	}

	std::size_t count(std::string_view part) const
	{
		std::size_t n = 0;
		for (const std::string& line : lines) {
			if (line.find(part) != std::string::npos) ++n;
		}
		return n;
	}

	std::vector<std::string> lines;
	engine::log_sink_handle sink;
};

}

BOOST_FIXTURE_TEST_SUITE(ResSyncRequireWarningTests, test_util::ram_res_env)

BOOST_AUTO_TEST_CASE(ARequireSyncOfAnExtensionThatCanBlock_WarnsOncePerTag)
{
	rs->warn_on_sync_require({ "glb", "obj" });
	const res::tag robot = put("robot.glb", "model");
	const res::tag shouted = put("crate.OBJ", "model");
	const res::tag text = put("notes.txt", "text");

	warnings logs;
	rs->require_sync<res::text_resource>(robot);
	rs->require_sync<res::text_resource>(robot);
	rs->require_sync<res::text_resource>(shouted);
	rs->require_sync<res::text_resource>(text);

	BOOST_TEST(logs.count("require_sync('memory://t/robot.glb') blocks the main thread") == 1u);
	BOOST_TEST(logs.count("require_sync('memory://t/crate.OBJ')") == 1u);
	BOOST_TEST(logs.count("notes.txt") == 0u);
}

BOOST_AUTO_TEST_CASE(WithoutExtensionsThatCanBlock_NothingWarns)
{
	const res::tag robot = put("robot.glb", "model");
	warnings logs;
	rs->require_sync<res::text_resource>(robot);
	BOOST_TEST(logs.count("blocks the main thread") == 0u);
}

BOOST_AUTO_TEST_CASE(ALayerWarnsWithTheExtensionsOfItsRoot)
{
	rs->warn_on_sync_require({ "glb" });
	const res::tag robot = put("robot.glb", "model");
	res::resource_system layer(*rs);
	warnings logs;
	layer.require_sync<res::text_resource>(robot);
	rs->require_sync<res::text_resource>(robot);
	BOOST_TEST(logs.count("require_sync('memory://t/robot.glb')") == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
