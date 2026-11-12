#include <boost/test/unit_test.hpp>
#include <snk/game/gs_input_bindings_file.h>
#include <snk/input/inp_events.hpp>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// The player's bindings as a file (#736).

namespace {

using KB = inp::KEYBOARD_BUTTONS;
using MB = inp::MOUSE_BUTTONS;

struct temp_folder
{
	std::filesystem::path path;
	temp_folder()
	{
		static int counter = 0;
		path = std::filesystem::temp_directory_path() / ("snk_user_bindings_" + std::to_string(++counter) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		std::filesystem::create_directories(path);
	}
	~temp_folder()
	{
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};

}

BOOST_AUTO_TEST_SUITE(GsInputBindingsFileTests)

BOOST_AUTO_TEST_CASE(WhatIsWritten_ReadsBackTheSame)
{
	inp::user_bindings user;
	user.set("MoveLeft", { KB::Q, MB::RIGHT });
	user.set("Jump", {});
	const inp::user_bindings back = gs::parse_user_bindings(gs::write_user_bindings(user));
	BOOST_TEST((back.overrides() == user.overrides()));
}

BOOST_AUTO_TEST_CASE(ABindingThatIsWrong_IsSkippedAndTheRestLoads)
{
	const inp::user_bindings user = gs::parse_user_bindings(R"json({"__type":"user_input_bindings","__version":1,"actions":[
		{"name":"MoveLeft","bindings":[{"key":"NOPE"},{"key":"Q"},{"key":"Q","mouse":"LEFT"},{"mouse":7},{}]},
		{"name":"","bindings":[]},
		{"bindings":[]},
		{"name":"Fire","bindings":[{"mouse":"RIGHT"}]}]})json");
	BOOST_REQUIRE_EQUAL(user.overrides().size(), 2u);
	BOOST_TEST((user.overrides().at("MoveLeft") == std::vector<inp::action_binding>{ KB::Q }));
	BOOST_TEST((user.overrides().at("Fire") == std::vector<inp::action_binding>{ MB::RIGHT }));
}

BOOST_AUTO_TEST_CASE(TextThatIsNoSuchDocument_GivesNoBindings)
{
	BOOST_TEST(gs::parse_user_bindings("{ not json").empty());
	BOOST_TEST(gs::parse_user_bindings("[]").empty());
	BOOST_TEST(gs::parse_user_bindings(R"json({"__type":"input_actions_desc","actions":[{"name":"A","bindings":[]}]})json").empty());
	BOOST_TEST(gs::parse_user_bindings(R"json({"__type":"user_input_bindings","actions":5})json").empty());
}

BOOST_AUTO_TEST_CASE(AFileThatIsNotThere_ReadsAsNoBindings)
{
	temp_folder dir;
	BOOST_TEST(gs::load_user_bindings(dir.path / "missing.json").empty());
}

BOOST_AUTO_TEST_CASE(SaveCreatesTheFolder_ReplacesTheFile_AndLeavesNoTemporaryOne)
{
	temp_folder dir;
	const auto file = dir.path / "settings" / "game.json";
	inp::user_bindings first;
	first.set("MoveLeft", { KB::Q });
	BOOST_REQUIRE(gs::save_user_bindings(file, first));
	inp::user_bindings second;
	second.set("Fire", { MB::RIGHT });
	BOOST_REQUIRE(gs::save_user_bindings(file, second));

	BOOST_TEST((gs::load_user_bindings(file).overrides() == second.overrides()));
	BOOST_TEST(!std::filesystem::exists(dir.path / "settings" / "game.json.tmp"));
}

BOOST_AUTO_TEST_CASE(MoreThanTheLimitOfActionsAndBindings_IsCut)
{
	std::string text = R"json({"__type":"user_input_bindings","__version":1,"actions":[)json";
	for (int i = 0; i < 300; ++i) {
		text += std::string(i ? "," : "") + "{\"name\":\"A" + std::to_string(i) + "\",\"bindings\":[";
		for (int j = 0; j < 20; ++j) text += std::string(j ? "," : "") + R"json({"key":"Q"})json";
		text += "]}";
	}
	text += "]}";
	const inp::user_bindings user = gs::parse_user_bindings(text);
	BOOST_TEST(user.overrides().size() == 256u);
	BOOST_TEST(user.overrides().at("A0").size() == 16u);
}

BOOST_AUTO_TEST_CASE(AFileOverTheLimit_GivesNoBindings)
{
	temp_folder dir;
	const auto file = dir.path / "big.json";
	std::ofstream(file) << R"json({"__type":"user_input_bindings","__version":1,"actions":[],"pad":")json" << std::string(300 * 1024, 'x') << "\"}";
	BOOST_TEST(gs::load_user_bindings(file).empty());
}

BOOST_AUTO_TEST_CASE(AFileThatIsALink_IsNotWrittenThrough)
{
	temp_folder dir;
	const auto target = dir.path / "target.txt";
	std::ofstream(target) << "KEEP";
	const auto link = dir.path / "bindings" / "game.json";
	std::filesystem::create_directories(link.parent_path());
	std::error_code ec;
	std::filesystem::create_symlink(target, link, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("no symbolic links here: " << ec.message());
		return;
	}
	inp::user_bindings user;
	user.set("Fire", { MB::RIGHT });
	BOOST_TEST(!gs::save_user_bindings(link, user));

	std::string kept;
	std::ifstream(target) >> kept;
	BOOST_TEST(kept == "KEEP");
}

BOOST_AUTO_TEST_CASE(ABrokenFileOnDisk_GivesNoBindings)
{
	temp_folder dir;
	const auto file = dir.path / "broken.json";
	std::ofstream(file) << "{ \"__type\": ";
	BOOST_TEST(gs::load_user_bindings(file).empty());
}

BOOST_AUTO_TEST_SUITE_END()
