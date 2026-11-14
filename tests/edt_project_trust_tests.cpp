#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include "edt_project_trust.h"
#include "edt_project_trust_service.h"
#include "edt_settings.h"
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_dialog_service.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// #921: the person's trust in a project, kept in edt://settings/project_trust.json by the canonical project path.

namespace {

	constexpr std::string_view k_project = "C:/work/game";
	constexpr std::string_view k_other = "C:/work/other";
	const std::string k_hash_a(64, 'a');
	const std::string k_hash_b(64, 'b');

	// A library as the loader names it on this platform (core copies and loads only those).
	std::string library_name(std::string_view module)
	{
#if defined(_WIN32)
		return std::string(module) + ".dll";
#else
		return "lib" + std::string(module) + ".so";
#endif
	}
	std::filesystem::path fs_bin(std::string_view file) { return std::filesystem::path("build/bin") / file; }
	std::filesystem::path fs_copy(std::string_view file) { return std::filesystem::path("cache/1-0") / file; }

	// edt://settings in a scratch folder. The temp dir first: the resource system's io thread is joined before it goes.
	struct settings_env
	{
		settings_env()
		{
			const std::filesystem::path data = dir.path() / "data";
			std::filesystem::create_directories(data);
			BOOST_REQUIRE(edt::register_data_protocol(*env.rs, data, dir.path() / "user"));
		}

		std::shared_ptr<edt::project_trust_service> service(std::optional<std::string> key = std::string(k_project))
		{
			return std::make_shared<edt::project_trust_service>(*env.rs, std::move(key));
		}

		test_util::temp_dir dir{ "edt_project_trust" };
		test_util::ram_res_env env;
	};

}

BOOST_AUTO_TEST_SUITE(EdtProjectTrustTests)

BOOST_AUTO_TEST_CASE(AnAnswer_IsSetAndForgotten_PerProject)
{
	std::string text = edt::with_trust_answer(std::nullopt, k_project, edt::trust_answer::trusted);
	BOOST_CHECK(edt::project_trust_of(text, k_project).answer == edt::trust_answer::trusted);
	BOOST_CHECK(edt::project_trust_of(text, k_other).answer == edt::trust_answer::none);

	text = edt::with_trust_answer(text, k_other, edt::trust_answer::refused);
	BOOST_CHECK(edt::project_trust_of(text, k_other).answer == edt::trust_answer::refused);
	text = edt::with_trust_answer(text, k_project, edt::trust_answer::none);
	BOOST_CHECK(edt::project_trust_of(text, k_project).answer == edt::trust_answer::none);
	BOOST_CHECK(edt::project_trust_of(text, k_other).answer == edt::trust_answer::refused);
}

BOOST_AUTO_TEST_CASE(OwnBuilds_AreKeptAcrossAnswerChanges)
{
	std::string text = edt::with_own_build(std::nullopt, k_project, "game", k_hash_a);
	text = edt::with_trust_answer(text, k_project, edt::trust_answer::refused);
	text = edt::with_trust_answer(text, k_project, edt::trust_answer::none);
	text = edt::with_own_build(text, k_project, "game_editor", k_hash_b);
	const edt::project_trust_record record = edt::project_trust_of(text, k_project);
	BOOST_CHECK(record.answer == edt::trust_answer::none);
	BOOST_REQUIRE_EQUAL(record.own_builds.size(), 2u);
	BOOST_CHECK_EQUAL(record.own_builds.at("game"), k_hash_a);
	BOOST_CHECK_EQUAL(record.own_builds.at("game_editor"), k_hash_b);
	BOOST_CHECK(edt::project_trust_of(text, k_other).own_builds.empty());
}

BOOST_AUTO_TEST_CASE(UnreadableSettings_GiveAnEmptyRecord)
{
	for (const std::string bad : { "", "not json", "[]", R"({"projects":[]})", R"({"projects":{"C:/work/game":1}})",
			R"({"projects":{"C:/work/game":{"trust":"yes","own":[]}}})" }) {
		BOOST_TEST_CONTEXT(bad) {
			const edt::project_trust_record record = edt::project_trust_of(bad, k_project);
			BOOST_CHECK(record.answer == edt::trust_answer::none);
			BOOST_CHECK(record.own_builds.empty());
			const std::string fixed = edt::with_trust_answer(bad, k_project, edt::trust_answer::trusted);
			BOOST_CHECK(edt::project_trust_of(fixed, k_project).answer == edt::trust_answer::trusted);
		}
	}
	BOOST_CHECK(edt::project_trust_of(std::nullopt, k_project).answer == edt::trust_answer::none);
}

BOOST_AUTO_TEST_CASE(JudgeLibrary_Matrix)
{
	using edt::library_verdict;
	using edt::trust_answer;
	const std::optional<std::string> own = k_hash_a;
	const std::optional<std::string> other = k_hash_b;
	const std::optional<std::string> unreadable = std::nullopt;
	struct row
	{
		trust_answer answer;
		std::optional<std::string> sha256;
		library_verdict expected;
	};
	const std::vector<row> rows{
		{ trust_answer::trusted, own, library_verdict::load },
		{ trust_answer::trusted, other, library_verdict::load },
		{ trust_answer::trusted, unreadable, library_verdict::load },
		{ trust_answer::refused, own, library_verdict::refuse },
		{ trust_answer::refused, other, library_verdict::refuse },
		{ trust_answer::refused, unreadable, library_verdict::refuse },
		{ trust_answer::none, own, library_verdict::load },
		{ trust_answer::none, other, library_verdict::ask },
		{ trust_answer::none, unreadable, library_verdict::ask },
	};
	for (std::size_t i = 0; i < rows.size(); ++i) {
		BOOST_TEST_CONTEXT("row " << i) {
			edt::project_trust_record record;
			record.answer = rows[i].answer;
			record.own_builds.emplace("game", k_hash_a);
			BOOST_CHECK(edt::judge_library(record, "game", rows[i].sha256) == rows[i].expected);
		}
	}
	// Another module's own build does not stand for this one.
	edt::project_trust_record record;
	record.own_builds.emplace("other", k_hash_a);
	BOOST_CHECK(edt::judge_library(record, "game", own) == library_verdict::ask);
}

BOOST_AUTO_TEST_CASE(TheStore_RoundTripsThroughThePersonsSettings)
{
	settings_env settings;
	BOOST_CHECK(edt::read_project_trust(*settings.env.rs, k_project).answer == edt::trust_answer::none);
	BOOST_CHECK(!edt::store_trust_answer(*settings.env.rs, k_project, edt::trust_answer::trusted));
	BOOST_CHECK(!edt::store_own_build(*settings.env.rs, k_other, "game", k_hash_a));
	BOOST_CHECK(edt::read_project_trust(*settings.env.rs, k_project).answer == edt::trust_answer::trusted);
	BOOST_CHECK(edt::read_project_trust(*settings.env.rs, k_other).answer == edt::trust_answer::none);
	BOOST_CHECK_EQUAL(edt::read_project_trust(*settings.env.rs, k_other).own_builds.at("game"), k_hash_a);
	const std::filesystem::path file = settings.dir.path() / "user" / "settings"
		/ std::string{ edt::k_project_trust_settings };
	BOOST_CHECK(std::filesystem::is_regular_file(file));
}

// A foreign project's library without an answer waits for the person and is not loaded.
BOOST_AUTO_TEST_CASE(AForeignLibrary_WaitsForTheAnswer)
{
	settings_env settings;
	const auto trust = settings.service();
	const std::filesystem::path copy = settings.dir.write("cache/libgame.so", "foreign code");
	BOOST_CHECK(!trust->trusted());
	BOOST_CHECK(!trust->allow_library("game", copy));
	BOOST_CHECK(!trust->allow_library("game", copy));
	BOOST_REQUIRE(trust->waiting());
	BOOST_TEST((trust->waiting_modules() == std::vector<std::string>{ "game" }));
}

// Held modules, waiting or refused, are named in the menu bar until they load (#921 review).
BOOST_AUTO_TEST_CASE(HeldModules_AreShownUntilTheyLoad)
{
	settings_env settings;
	const std::filesystem::path copy = settings.dir.write("cache/libgame.so", "foreign code");
	const auto trust = settings.service();
	BOOST_CHECK(!edt::held_modules_status(trust->held_modules()));
	BOOST_CHECK(!trust->allow_library("game", copy));
	BOOST_CHECK(!trust->allow_library("game_editor", copy));
	trust->answer(edt::trust_answer::refused);
	BOOST_CHECK(!trust->allow_library("game", copy));
	BOOST_TEST((trust->held_modules() == std::vector<std::string>{ "game", "game_editor" }));
	const std::optional<std::string> status = edt::held_modules_status(trust->held_modules());
	BOOST_REQUIRE(status.has_value());
	BOOST_TEST(status->find("game, game_editor") != std::string::npos);
	BOOST_TEST(status->find("Project > Trust Project Modules") != std::string::npos);

	trust->answer(edt::trust_answer::trusted);
	BOOST_CHECK(trust->allow_library("game", copy));
	BOOST_TEST((trust->held_modules() == std::vector<std::string>{ "game_editor" }));
}

// The answer is stored per project key and read back by the next session.
BOOST_AUTO_TEST_CASE(Trust_IsPersistedPerProject)
{
	settings_env settings;
	const std::filesystem::path copy = settings.dir.write("cache/libgame.so", "foreign code");
	settings.service()->answer(edt::trust_answer::trusted);

	const auto next = settings.service();
	BOOST_CHECK(next->trusted());
	BOOST_CHECK(next->allow_library("game", copy));
	BOOST_CHECK(!next->waiting());

	const auto other = settings.service(std::string(k_other));
	BOOST_CHECK(!other->trusted());
	BOOST_CHECK(!other->allow_library("game", copy));

	settings.service(std::string(k_other))->answer(edt::trust_answer::refused);
	const auto refused = settings.service(std::string(k_other));
	BOOST_CHECK(!refused->allow_library("game", copy));
	BOOST_CHECK(!refused->waiting());
}

// What the editor built loads without a question, in this session and the next; a changed byte is asked again.
BOOST_AUTO_TEST_CASE(AnOwnBuild_LoadsWithoutAsking_UntilItChanges)
{
	settings_env settings;
	const std::filesystem::path built = settings.dir.write(fs_bin(library_name("game")), "built by the editor");
	const std::filesystem::path copy = settings.dir.write(fs_copy(library_name("game")), "built by the editor");
	settings.service()->record_own_build("game", built);

	const auto trust = settings.service();
	BOOST_CHECK(trust->allow_library("game", copy));
	BOOST_CHECK(!trust->waiting());
	BOOST_CHECK(!trust->trusted());

	settings.dir.write(fs_copy(library_name("game")), "built by the editoR");
	BOOST_CHECK(!trust->allow_library("game", copy));
	BOOST_TEST((trust->waiting_modules() == std::vector<std::string>{ "game" }));

	// A digest handed over by the builder counts the same.
	const std::optional<std::string> digest = core::module_library_set_digest(copy.parent_path());
	BOOST_REQUIRE(digest.has_value());
	trust->record_own_build_digest("game", *digest);
	trust->clear_waiting();
	BOOST_CHECK(trust->allow_library("game", copy));
}

// #921 review: the loader also loads the libraries the copy takes along, so a library planted next to the editor's
// own build is asked about; debug symbols are not loaded and do not count.
BOOST_AUTO_TEST_CASE(ALibraryPlantedBesideAnOwnBuild_IsAskedAbout)
{
	settings_env settings;
	const std::filesystem::path built = settings.dir.write(fs_bin(library_name("game")), "built by the editor");
	const std::filesystem::path copy = settings.dir.write(fs_copy(library_name("game")), "built by the editor");
	settings.service()->record_own_build("game", built);
#if defined(_WIN32)
	settings.dir.write(fs_copy("game.pdb"), "symbols of another build");
#endif
	const auto trust = settings.service();
	BOOST_CHECK(trust->allow_library("game", copy));

	settings.dir.write(fs_copy(library_name("version")), "planted");
	BOOST_CHECK(!trust->allow_library("game", copy));
	BOOST_TEST((trust->waiting_modules() == std::vector<std::string>{ "game" }));
}

// A build only --trust-project allowed is not remembered as the person's: the next run without the flag asks.
BOOST_AUTO_TEST_CASE(AnOwnBuild_IsNotRecorded_WhenOnlyTheFlagTrusts)
{
	settings_env settings;
	const std::filesystem::path built = settings.dir.write(fs_bin(library_name("game")), "built by the editor");
	const std::filesystem::path copy = settings.dir.write(fs_copy(library_name("game")), "built by the editor");
	const auto flagged = settings.service();
	flagged->set_flag_trust(true);
	flagged->record_own_build("game", built);
	BOOST_CHECK(settings.service()->current_answer() == edt::trust_answer::none);
	BOOST_CHECK(!settings.service()->allow_library("game", copy));

	const auto trusted = settings.service();
	trusted->answer(edt::trust_answer::trusted);
	trusted->set_flag_trust(true);
	trusted->record_own_build("game", built);
	trusted->answer(edt::trust_answer::none);
	BOOST_CHECK(settings.service()->allow_library("game", copy));
}

// --trust-project: everything loads, nothing is stored; without a project key only the flag trusts.
BOOST_AUTO_TEST_CASE(TheFlag_TrustsForThisRunOnly)
{
	settings_env settings;
	const std::filesystem::path copy = settings.dir.write("cache/libgame.so", "foreign code");
	const auto flagged = settings.service();
	flagged->set_flag_trust(true);
	BOOST_CHECK(flagged->trusted());
	BOOST_CHECK(flagged->allow_library("game", copy));
	BOOST_CHECK(!settings.service()->trusted());

	const auto keyless = settings.service(std::nullopt);
	BOOST_CHECK(!keyless->has_project());
	BOOST_CHECK(!keyless->allow_library("game", copy));
	keyless->set_flag_trust(true);
	BOOST_CHECK(keyless->allow_library("game", copy));
}

// The dialog stores "Trust" / "Don't Trust"; a cancel stores nothing; one question at a time.
BOOST_AUTO_TEST_CASE(TheQuestion_StoresOnlyAnAnswer)
{
	settings_env settings;
	edt::dialog_service dialogs;
	dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	const auto trust = settings.service();
	std::vector<edt::trust_answer> answers;
	const auto record = [&answers](edt::trust_answer answer) { answers.push_back(answer); };

	edt::ask_project_trust(dialogs, trust, record);
	edt::ask_project_trust(dialogs, trust, record);
	BOOST_REQUIRE_EQUAL(dialogs.list().size(), 1u);
	BOOST_CHECK(dialogs.answer(dialogs.list().front().id, boost::json::object{ { "cancel", true } }).ok);
	BOOST_CHECK(answers.empty());
	BOOST_CHECK(settings.service()->current_answer() == edt::trust_answer::none);

	edt::ask_project_trust(dialogs, trust, record);
	BOOST_REQUIRE_EQUAL(dialogs.list().size(), 1u);
	// "Don't Trust" comes first.
	BOOST_CHECK(dialogs.answer(dialogs.list().front().id, boost::json::object{ { "index", 0 } }).ok);
	BOOST_TEST((answers == std::vector<edt::trust_answer>{ edt::trust_answer::refused }));
	BOOST_CHECK(settings.service()->current_answer() == edt::trust_answer::refused);

	edt::ask_project_trust(dialogs, trust, record);
	BOOST_CHECK(dialogs.answer(dialogs.list().front().id, boost::json::object{ { "index", 1 } }).ok);
	BOOST_CHECK(answers.back() == edt::trust_answer::trusted);
	BOOST_CHECK(settings.service()->trusted());
}

BOOST_AUTO_TEST_CASE(TheQuestion_NamesTheWaitingModules)
{
	const std::string text = edt::project_trust_question({ "game", "game_editor" }, edt::trust_answer::none);
	BOOST_CHECK(text.find("Its code will run") != std::string::npos);
	BOOST_CHECK(text.find("game, game_editor") != std::string::npos);
	BOOST_CHECK(text.find("never in the project") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
