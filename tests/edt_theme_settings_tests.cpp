#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "settings/edt_theme_settings.h"
#include <snk/gui/gui_gui_service_init.h>
#include <snk/gui/gui_theme_service.h>

#include <boost/json.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// The person's theme (#1109): the choice kept in edt://settings/, presets duplicated and saved into edt://themes/,
// applied through gui::theme_service. edt:// and engine:// are RAM here; core/res is the engine's.

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> read_bytes(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<char> raw{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::vector<std::byte> out(raw.size());
	std::memcpy(out.data(), raw.data(), raw.size());
	return out;
}

std::string text_of(const std::vector<std::byte>& bytes)
{
	return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

struct theme_settings_env : test_util::ram_res_env
{
	desc::desc_system descs{ *rs };
	test_util::ram_resolver* engine_ram = nullptr;
	test_util::ram_resolver* edt_ram = nullptr;
	std::vector<gui::theme_service::theme> applied;
	gui::theme_service themes{ *rs, res::tag{ "engine://themes/classic.desc" },
		[this](gui::theme_service::theme theme) { applied.push_back(std::move(theme)); } };

	theme_settings_env()
	{
		rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs),
			std::ref(descs));
		engine::gui::register_gui_descs(descs);
		engine_ram = &rs->registrate_resolver<test_util::ram_resolver>(res::tag::engine);
		edt_ram = &rs->registrate_resolver<test_util::ram_resolver>("edt");
		for (const std::string_view folder : { "themes", "fonts" }) {
			for (const auto& entry : fs::directory_iterator(fs::path{ SNK_CORE_RES_DIR } / folder)) {
				const std::string relative = std::string{ folder } + "/" + entry.path().filename().generic_string();
				engine_ram->files[res::tag{ res::tag::engine, relative }] = read_bytes(entry.path());
			}
		}
	}

	~theme_settings_env()
	{
		engine::gui::unregister_gui_descs(descs);
		rs->unregistrate_adapter(core::desc::desc_adapter_t::INFO);
	}

	edt::theme_settings_model make_model(bool set_by_config = false)
	{
		return edt::theme_settings_model{ { .resources = *rs, .descs = descs, .themes = &themes,
			.theme_set_by_config = set_by_config,
			.list_files = [this](std::string_view protocol, std::string_view folder) {
				return list(protocol, folder);
			} } };
	}

	// What files_under gives for a disk mount, from the RAM resolvers.
	std::vector<res::tag> list(std::string_view protocol, std::string_view folder)
	{
		const test_util::ram_resolver* ram = protocol == "edt" ? edt_ram : engine_ram;
		std::vector<res::tag> out;
		const std::string prefix = std::string{ protocol } + "://" + std::string{ folder } + "/";
		for (const auto& [tag, bytes] : ram->files) {
			if (tag.view().starts_with(prefix)) out.push_back(tag);
		}
		return out;
	}

	void settle(edt::theme_settings_model& model)
	{
		for (int i = 0; i < 50; ++i) {
			pump();
			themes.poll();
			model.poll();
		}
	}

	std::string edt_file(std::string_view path)
	{
		const auto it = edt_ram->files.find(res::tag{ "edt", path });
		return it == edt_ram->files.end() ? std::string{} : text_of(it->second);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtThemeSettingsTests, theme_settings_env)

BOOST_AUTO_TEST_CASE(TheChoiceIsAThemeKeyInAJsonObject)
{
	BOOST_TEST(edt::chosen_theme_of(R"({"theme":"edt://themes/a.desc"})")->view() == "edt://themes/a.desc");
	BOOST_TEST(!edt::chosen_theme_of("").has_value());
	BOOST_TEST(!edt::chosen_theme_of("not json").has_value());
	BOOST_TEST(!edt::chosen_theme_of(R"({"theme":3})").has_value());
	BOOST_TEST(!edt::chosen_theme_of(R"(["theme"])").has_value());

	const std::string kept = edt::with_chosen_theme(R"({"other":1})", res::tag{ "engine://themes/light.desc" });
	BOOST_TEST(edt::chosen_theme_of(kept)->view() == "engine://themes/light.desc");
	BOOST_TEST(kept.find("other") != std::string::npos);
	BOOST_TEST(edt::chosen_theme_of(edt::with_chosen_theme("broken {", res::tag{ "engine://x.desc" })).has_value());
}

BOOST_AUTO_TEST_CASE(ANameOfAPersonalPreset_IsAPlainFileName)
{
	BOOST_TEST(edt::valid_theme_name("my_dark-2"));
	BOOST_TEST(!edt::valid_theme_name(""));
	BOOST_TEST(!edt::valid_theme_name("../settings/x"));
	BOOST_TEST(!edt::valid_theme_name("a b"));
	BOOST_TEST(!edt::valid_theme_name("a.desc"));
	BOOST_TEST(!edt::valid_theme_name(std::string(65, 'a')));
	BOOST_TEST(!edt::valid_theme_name("con"));
	BOOST_TEST(!edt::valid_theme_name("COM1"));
	BOOST_TEST(edt::valid_theme_name("console"));
	BOOST_TEST(edt::personal_theme_tag("mine").view() == "edt://themes/mine.desc");
	BOOST_TEST(edt::is_personal_theme(edt::personal_theme_tag("mine")));
	BOOST_TEST(!edt::is_personal_theme(res::tag{ "engine://themes/light.desc" }));
	BOOST_TEST(!edt::is_personal_theme(res::tag{ "edt://themes/../settings/x.desc" }));
	BOOST_TEST(edt::theme_name_of(res::tag{ "engine://themes/dark_neutral.desc" }) == "dark_neutral");
}

BOOST_AUTO_TEST_CASE(TheKeptChoice_IsAppliedAtStart)
{
	edt_ram->files[res::tag{ "edt://settings/editor_theme.json" }] =
		test_util::to_bytes(R"({"theme":"engine://themes/light.desc"})");
	auto model = make_model();

	model.start();
	settle(model);

	BOOST_TEST(themes.selected().view() == "engine://themes/light.desc");
	BOOST_REQUIRE(!applied.empty());
	BOOST_TEST(applied.back().style.palette.light);
	BOOST_REQUIRE(model.working() != nullptr);
	BOOST_TEST(!model.editable());
}

BOOST_AUTO_TEST_CASE(AKeptChoiceOutsideTheEngineAndThePersonsThemes_IsNotRead)
{
	edt_ram->files[res::tag{ "edt://settings/editor_theme.json" }] =
		test_util::to_bytes(R"({"theme":"memory://t/project_theme.desc"})");
	put("project_theme.desc", R"({ "__type" : "gui.theme", "__version" : 1 })");
	auto model = make_model();

	model.start();
	settle(model);

	BOOST_TEST(themes.selected().view() != "memory://t/project_theme.desc");
	BOOST_TEST(model.working_tag().view() != "memory://t/project_theme.desc");
	BOOST_TEST(model.working() == nullptr);
}

BOOST_AUTO_TEST_CASE(AChoiceMadeWhileTheKeptOneIsRead_IsNotUndoneByIt)
{
	edt_ram->files[res::tag{ "edt://settings/editor_theme.json" }] =
		test_util::to_bytes(R"({"theme":"engine://themes/dark_contrast.desc"})");
	auto model = make_model();
	model.start();
	pump();

	model.choose(res::tag{ "engine://themes/light.desc" });
	settle(model);

	BOOST_TEST(themes.selected().view() == "engine://themes/light.desc");
	BOOST_REQUIRE(model.working() != nullptr);
	BOOST_TEST(model.working_tag().view() == "engine://themes/light.desc");
}

BOOST_AUTO_TEST_CASE(AThemeNamedByTheConfig_WinsOverTheKeptChoice)
{
	edt_ram->files[res::tag{ "edt://settings/editor_theme.json" }] =
		test_util::to_bytes(R"({"theme":"engine://themes/light.desc"})");
	themes.select(res::tag{ "engine://themes/dark_contrast.desc" });
	auto model = make_model(true);

	model.start();
	settle(model);

	BOOST_TEST(themes.selected().view() == "engine://themes/dark_contrast.desc");
}

BOOST_AUTO_TEST_CASE(TheListHasTheShippedPresetsThenThePersons)
{
	edt_ram->files[res::tag{ "edt://themes/zz_mine.desc" }] =
		test_util::to_bytes(R"({"__type":"gui.theme","__version":1})");
	edt_ram->files[res::tag{ "edt://fonts/own.TTF" }] = test_util::to_bytes("x");
	edt_ram->files[res::tag{ "edt://fonts/readme.txt" }] = test_util::to_bytes("x");
	auto model = make_model();

	model.refresh_lists();

	BOOST_REQUIRE(model.presets().size() >= 5u);
	BOOST_TEST(model.presets().front().view().starts_with("engine://themes/"));
	BOOST_TEST(model.presets().back().view() == "edt://themes/zz_mine.desc");
	BOOST_REQUIRE_EQUAL(model.fonts().size(), 2u);
	BOOST_TEST(model.fonts().back().view() == "edt://fonts/own.TTF");
}

BOOST_AUTO_TEST_CASE(ADuplicate_IsWrittenToEdtThemes_ChosenAndKept)
{
	auto model = make_model();
	model.start();
	model.choose(res::tag{ "engine://themes/dark_neutral.desc" });
	settle(model);
	BOOST_REQUIRE(model.working() != nullptr);

	BOOST_TEST(!model.duplicate("mine").has_value());
	settle(model);

	const boost::json::value file = boost::json::parse(edt_file("themes/mine.desc"));
	BOOST_TEST(file.at("__type").as_string() == "gui.theme");
	BOOST_TEST(file.at("__version").as_int64() == 1);
	BOOST_TEST(file.at("base").as_string() == "palette");
	BOOST_TEST(themes.selected().view() == "edt://themes/mine.desc");
	BOOST_TEST(edt::chosen_theme_of(edt_file("settings/editor_theme.json"))->view() == "edt://themes/mine.desc");
	BOOST_REQUIRE(model.working() != nullptr);
	BOOST_TEST(model.editable());

	BOOST_TEST(model.duplicate("mine").has_value());
	BOOST_TEST(model.duplicate("../escape").has_value());
	BOOST_TEST(edt_file("escape.desc").empty());
}

BOOST_AUTO_TEST_CASE(AnEditIsShownAtOnce_AndWrittenOnlyBySave)
{
	auto model = make_model();
	model.start();
	model.choose(res::tag{ "engine://themes/dark_neutral.desc" });
	settle(model);
	BOOST_REQUIRE(!model.duplicate("mine").has_value());
	settle(model);
	BOOST_REQUIRE(model.working() != nullptr);
	const std::string saved = edt_file("themes/mine.desc");

	model.working()->accent = glm::vec3{ 1.f, 0.f, 0.f };
	applied.clear();
	model.edited();
	settle(model);

	BOOST_REQUIRE(!applied.empty());
	BOOST_TEST(applied.back().style.palette.accent.x == 1.f);
	BOOST_TEST(!applied.back().fonts_changed);
	BOOST_TEST(model.dirty());
	BOOST_TEST(edt_file("themes/mine.desc") == saved);

	BOOST_TEST(!model.save().has_value());
	BOOST_TEST(!model.dirty());
	const boost::json::value file = boost::json::parse(edt_file("themes/mine.desc"));
	BOOST_TEST(file.at("accent").as_array().at(0).to_number<double>() == 1.0);
}

BOOST_AUTO_TEST_CASE(ABuiltInPreset_IsNotSaved)
{
	auto model = make_model();
	model.start();
	model.choose(res::tag{ "engine://themes/light.desc" });
	settle(model);
	BOOST_REQUIRE(model.working() != nullptr);

	BOOST_TEST(model.save().has_value());
	BOOST_TEST((edt_ram->files.empty() || edt_file("themes/light.desc").empty()));
}

BOOST_AUTO_TEST_CASE(WithoutARenderer_NothingIsAppliedOrWritten)
{
	edt::theme_settings_model model{ { .resources = *rs, .descs = descs, .themes = nullptr,
		.theme_set_by_config = false, .list_files = {} } };

	model.start();
	model.choose(res::tag{ "engine://themes/light.desc" });

	BOOST_TEST(!model.available());
	BOOST_TEST(edt_file("settings/editor_theme.json").empty());
}

BOOST_AUTO_TEST_SUITE_END()
