#include <boost/test/unit_test.hpp>
#include <snk/game/gs_input_bindings_file.h>
#include "test_scene_env.hpp"
#include <snk/desc/desc_system.h>
#include <snk/input/inp_events.hpp>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <boost/json.hpp>
#include <string>
#include <string_view>
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
	user.set("MoveLeft", { KB::Q, MB::RIGHT, inp::GAMEPAD_BUTTONS::DPAD_LEFT, inp::GAMEPAD_AXES::LEFT_X_NEG });
	user.set("Jump", {});
	const inp::user_bindings back = gs::parse_user_bindings(gs::write_user_bindings(user));
	BOOST_TEST((back.overrides() == user.overrides()));
}

BOOST_AUTO_TEST_CASE(AThresholdOfAGamepadBinding_IsWrittenAndRead)
{
	inp::user_bindings user;
	user.set("Aim", { inp::action_binding{ inp::GAMEPAD_AXES::RIGHT_TRIGGER, 0.35f }, inp::action_binding{ inp::GAMEPAD_BUTTONS::A }, KB::Q });
	const inp::user_bindings back = gs::parse_user_bindings(gs::write_user_bindings(user));
	BOOST_REQUIRE_EQUAL(back.overrides().at("Aim").size(), 3u);
	BOOST_TEST(back.overrides().at("Aim")[0].threshold() == 0.35f);
	BOOST_TEST(back.overrides().at("Aim")[1].threshold() == 0.0f);
	BOOST_TEST((back.overrides() == user.overrides()));
	const std::string text = gs::write_user_bindings(user);
	const auto written = boost::json::parse(text);
	const auto& first = written.at("actions").as_array().at(0).at("bindings").as_array().at(0).as_object();
	BOOST_TEST_INFO("file: " << text);
	// Boost.JSON prints 0.35 as 3.5E-1: the number is what counts, and a float widened to a double is 6e-9 away from it.
	BOOST_CHECK_SMALL(first.at("threshold").to_number<double>() - 0.35, 1e-12);
}

BOOST_AUTO_TEST_CASE(ABindingThatIsWrong_IsSkippedAndTheRestLoads)
{
	const inp::user_bindings user = gs::parse_user_bindings(R"json({"__type":"user_input_bindings","__version":1,"actions":[
		{"name":"MoveLeft","bindings":[{"key":"NOPE"},{"key":"Q"},{"key":"Q","mouse":"LEFT"},{"mouse":7},{},{"gamepad":"TURBO"},{"axis":"LeftX+"},{"gamepad":"A"}]},
		{"name":"","bindings":[]},
		{"bindings":[]},
		{"name":"Fire","bindings":[{"mouse":"RIGHT"}]}]})json");
	BOOST_REQUIRE_EQUAL(user.overrides().size(), 2u);
	BOOST_TEST((user.overrides().at("MoveLeft") == std::vector<inp::action_binding>{ KB::Q, inp::GAMEPAD_AXES::LEFT_X_POS, inp::GAMEPAD_BUTTONS::A }));
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

// #766 (EPIC-58 section 7.2): an older file is brought up in memory by the steps of the desc system; a newer one gives
// no bindings and is newer, one that is no bindings document of a version this engine reads is unreadable.
BOOST_FIXTURE_TEST_CASE(AnOlderFile_IsMigratedInMemory_ANewerOneIsNewer_ABrokenOneUnreadable, test_util::scene_env)
{
	using state = gs::user_file_state;
	// The format moves to 2 in this test: its step renames "keys" to "actions".
	descs->register_migration("user_input_bindings", 1, [](boost::json::object& o) {
		if (const auto* keys = o.if_contains("keys")) {
			boost::json::value moved = *keys;
			o.erase("keys");
			o["actions"] = std::move(moved);
		}
	});
	state got = state::newer;
	const inp::user_bindings older = gs::parse_user_bindings(
		R"({"__type":"user_input_bindings","__version":1,"keys":[{"name":"Jump","bindings":[{"key":"Q"}]}]})", "older.json", descs.get(), got);
	BOOST_TEST((got == state::usable));
	BOOST_TEST(older.overrides().size() == 1u);

	const inp::user_bindings newer = gs::parse_user_bindings(
		R"({"__type":"user_input_bindings","__version":3,"actions":[{"name":"Jump","bindings":[{"key":"Q"}]}]})", "newer.json", descs.get(), got);
	BOOST_TEST((got == state::newer));
	BOOST_TEST(newer.empty());

	for (const std::string_view text : { std::string_view{ R"({"__type":"user_input_bindings","__version":"x","actions":[]})" },
			std::string_view{ R"({"__type":"user_input_bindings","__version":0,"actions":[]})" }, std::string_view{ R"({"__type":"other"})" },
			std::string_view{ "{ not json" }, std::string_view{} }) {
		BOOST_TEST(gs::parse_user_bindings(text, "broken.json", descs.get(), got).empty(), text);
		BOOST_TEST((got == state::unreadable), text);
	}

	// Without a desc system only the version this engine writes is read.
	BOOST_TEST(gs::parse_user_bindings(R"({"__type":"user_input_bindings","__version":2,"actions":[]})", "two.json", nullptr, got).empty());
	BOOST_TEST((got == state::newer));
	gs::parse_user_bindings(R"({"__type":"user_input_bindings","__version":1,"actions":[]})", "one.json", nullptr, got);
	BOOST_TEST((got == state::usable));
}

namespace {

std::string read_all(const std::filesystem::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> files_in(const std::filesystem::path& folder)
{
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(folder)) files.push_back(entry.path());
	return files;
}

}

// #766, review of #1231: only a newer bindings document is kept as it is. An unreadable file is moved aside with its
// bytes, so the next save writes a new one; the plain load never moves a file.
BOOST_FIXTURE_TEST_CASE(OnLoad_ANewerFileIsKept_AnUnreadableOneIsMovedAside, test_util::scene_env)
{
	temp_folder dir;
	const auto file = dir.path / "player.json";
	const std::string newer = R"({"__type":"user_input_bindings","__version":2,"actions":[]})";
	std::ofstream(file, std::ios::binary) << newer;
	bool keep = false;
	BOOST_TEST(gs::load_user_bindings(file, descs.get(), keep).empty());
	BOOST_TEST(keep);
	BOOST_TEST(read_all(file) == newer);
	BOOST_TEST(files_in(dir.path).size() == 1u);

	for (const std::string& broken : { std::string{}, std::string{ R"({"__type":"user_input_bin)" }, std::string{ "not json" } }) {
		std::ofstream(file, std::ios::binary | std::ios::trunc) << broken;
		BOOST_TEST(gs::load_user_bindings(file).empty());
		BOOST_TEST(std::filesystem::exists(file));

		keep = true;
		BOOST_TEST(gs::load_user_bindings(file, descs.get(), keep).empty());
		BOOST_TEST(!keep);
		BOOST_TEST(!std::filesystem::exists(file));
		const std::vector<std::filesystem::path> files = files_in(dir.path);
		BOOST_REQUIRE(files.size() == 1u);
		BOOST_TEST(files[0].filename().string().starts_with("player.json.broken-"), files[0].filename().string());
		BOOST_TEST(read_all(files[0]) == broken);
		std::filesystem::remove(files[0]);

		BOOST_TEST(gs::save_user_bindings(file, inp::user_bindings{}));
		BOOST_TEST(gs::load_user_bindings(file, descs.get(), keep).empty());
		BOOST_TEST(!keep);
		std::filesystem::remove(file);
	}
}

BOOST_FIXTURE_TEST_CASE(TheVersionWritten_MustBeWhereTheStepsLead, test_util::scene_env)
{
	BOOST_TEST(gs::user_bindings_version_problem(*descs).empty());
	descs->register_migration("user_input_bindings", 1, [](boost::json::object&) {});
	const std::string problem = gs::user_bindings_version_problem(*descs);
	BOOST_TEST(problem.find("written at version 1") != std::string::npos, problem);
	BOOST_TEST(problem.find("lead to version 2") != std::string::npos, problem);
}

BOOST_AUTO_TEST_SUITE_END()
