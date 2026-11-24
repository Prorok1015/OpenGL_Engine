#include <boost/test/unit_test.hpp>
#include "png_bomb_builder.hpp"
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_file_menu.h"
#include "edt_image_import.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_file_change.h>
#include <boost/json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// The import of a picture with its settings (#1193): three new files in the project's top layer (the `.resource` first,
// the bytes as they are, the texture desc), the asset database keeps the sidecar's ID, and nothing stays when a write
// fails.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

std::string text_of(const json::value& value)
{
	return std::string(value.as_string());
}

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// A whole 2x2 RGBA picture: two rows of a filter byte and four pixels.
std::string small_png()
{
	const std::vector<unsigned char> rows{ 0, 10, 20, 30, 40, 50, 60, 70, 80, 0, 90, 100, 110, 120, 130, 140, 1, 2 };
	const std::vector<std::byte> bytes = test_util::png_bomb::png_rgba(2, 2, test_util::png_bomb::zlib_stream(rows, 0));
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct image_fixture
{
	image_fixture()
	{
		fs::create_directories(lower());
		fs::create_directories(top());
		source = fs::weakly_canonical(dir.write("outside/rock.png", png_text));
		importer = std::make_unique<edt::image_importer>(jobs, options());
	}

	fs::path lower() const { return dir.path() / "engine"; }
	fs::path top() const { return dir.path() / "project" / "res"; }

	edt::image_import_options options()
	{
		edt::image_import_options result;
		result.resource_layers = [this] { return std::vector<fs::path>{ lower(), top() }; };
		result.resources_path = [this] { return top(); };
		result.blocker = [this]() -> std::optional<std::string> { return blocked; };
		result.written = [this](const std::vector<res::tag>& tags) {
			reported = tags;
			if (db) {
				std::vector<res::file_change> changes;
				for (const res::tag& tag : tags) {
					res::file_change change;
					change.tag = tag;
					change.kind = res::change_kind::created;
					changes.push_back(std::move(change));
				}
				db->report_self_change(changes);
			}
		};
		result.now = [this] { return clock; };
		result.max_agent_bytes = 4096;
		result.fail_step = [this](std::string_view step) { return step == failing_step; };
		return result;
	}

	edt::image_import_request request(json::value settings = nullptr, std::optional<std::string> folder = std::nullopt,
		std::string name = {}) const
	{
		edt::image_import_request out;
		out.path = source.string();
		out.folder = std::move(folder);
		out.name = std::move(name);
		out.settings = std::move(settings);
		return out;
	}

	// The job of a person's import, run to its end.
	std::optional<edt::job_service::job_info> run(const edt::image_import_request& wanted)
	{
		const edt::image_importer::started started = importer->start(wanted, cmd::origin::ui(), false);
		BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
		test_util::pump(tasks);
		return jobs.info(started.job);
	}

	json::object imported(const edt::image_import_request& wanted)
	{
		const auto info = run(wanted);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, info->error);
		BOOST_REQUIRE(info->result.has_value());
		return info->result->as_object();
	}

	bool nothing_written(std::string_view folder = "textures") const
	{
		const fs::path where = top() / folder;
		std::error_code ec;
		return !fs::exists(where, ec) || fs::is_empty(where, ec);
	}

	std::string refusal_code(const edt::image_import_request& wanted) const
	{
		const edt::image_import_plan plan = importer->resolve(wanted, true);
		BOOST_CHECK(!plan.ok());
		return plan.code;
	}

	test_util::temp_dir dir{ "image_import" };
	const std::string png_text = small_png();
	fs::path source;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks };
	std::unique_ptr<edt::asset_database> db;
	std::unique_ptr<edt::image_importer> importer;
	std::optional<std::string> blocked;
	std::string failing_step;
	std::vector<res::tag> reported;
	std::chrono::steady_clock::time_point clock{};
	const cmd::origin agent = cmd::origin::agent("session-1");
};

}

BOOST_AUTO_TEST_SUITE(EdtImageImportSettingsTests)

BOOST_AUTO_TEST_CASE(MissingKeys_TakeTheDefaultsOfTheBaseTexture)
{
	const edt::image_settings_parse none = edt::parse_image_settings(nullptr);
	BOOST_REQUIRE(none.ok());
	BOOST_CHECK((none.settings == edt::image_import_settings{}));
	BOOST_CHECK((none.settings.wrap == edt::image_wrap::clamp_to_edge));
	BOOST_CHECK((none.settings.filter == edt::image_filter::nearest));
	BOOST_CHECK(!none.settings.mips);

	const edt::image_settings_parse some = edt::parse_image_settings(json::parse(R"({"version":1,"mips":true})"));
	BOOST_REQUIRE(some.ok());
	BOOST_CHECK(some.settings.mips);
	BOOST_CHECK((some.settings.wrap == edt::image_wrap::clamp_to_edge));
}

BOOST_AUTO_TEST_CASE(EveryValue_IsReadBack)
{
	const edt::image_settings_parse parsed = edt::parse_image_settings(
		json::parse(R"({"wrap":"repeat_mirror","filter":"linear","mips":false})"));
	BOOST_REQUIRE(parsed.ok());
	BOOST_CHECK((parsed.settings.wrap == edt::image_wrap::repeat_mirror));
	BOOST_CHECK((parsed.settings.filter == edt::image_filter::linear));
	BOOST_CHECK(edt::parse_image_settings(edt::image_settings_json(parsed.settings)).settings == parsed.settings);
}

BOOST_AUTO_TEST_CASE(AnUnknownKey_AWrongTypeOrValue_IsRefusedNamingTheKey)
{
	const auto error_of = [](std::string_view text) { return edt::parse_image_settings(json::parse(text)).error; };
	BOOST_CHECK(error_of(R"({"srgb":true})").find("settings.srgb") != std::string::npos);
	BOOST_CHECK(error_of(R"({"wrap":"mirror"})").find("settings.wrap") != std::string::npos);
	BOOST_CHECK(error_of(R"({"wrap":3})").find("settings.wrap") != std::string::npos);
	BOOST_CHECK(error_of(R"({"filter":"trilinear"})").find("settings.filter") != std::string::npos);
	BOOST_CHECK(error_of(R"({"mips":"yes"})").find("settings.mips") != std::string::npos);
	BOOST_CHECK(error_of(R"({"version":2})").find("settings.version") != std::string::npos);
	BOOST_CHECK(!error_of(R"([1])").empty());
}

BOOST_AUTO_TEST_CASE(TheTextureHeader_FollowsTheSettings_AndNamesNoTypeUsageExtentOrFormat)
{
	const res::tag image = res::tag::make("textures/rock.png");
	const json::object plain = edt::image_texture_desc_json(image, "rock.png", edt::image_import_settings{});
	BOOST_CHECK_EQUAL(text_of(plain.at("__type")), "texture_2d_desc");
	BOOST_CHECK_EQUAL(text_of(plain.at("__parent")), "res://base_texture.desc");
	BOOST_CHECK_EQUAL(text_of(plain.at("name")), "rock.png");
	BOOST_CHECK_EQUAL(text_of(plain.at("data")), "res://textures/rock.png");
	const json::object& header = plain.at("header").as_object();
	BOOST_CHECK_EQUAL(text_of(header.at("wrap")), "clamp_to_edge");
	BOOST_CHECK_EQUAL(text_of(header.at("mag")), "nearest");
	BOOST_CHECK_EQUAL(text_of(header.at("min")), "nearest");
	BOOST_CHECK_EQUAL(header.at("data").as_object().at("mip_levels").as_int64(), 1);
	for (const char* key : { "type", "usage" }) BOOST_CHECK(!header.contains(key));
	for (const char* key : { "extent", "format" }) BOOST_CHECK(!header.at("data").as_object().contains(key));

	edt::image_import_settings trilinear;
	trilinear.filter = edt::image_filter::linear;
	trilinear.wrap = edt::image_wrap::repeat;
	trilinear.mips = true;
	const json::object mipped = edt::image_texture_desc_json(image, "rock.png", trilinear);
	const json::object& mip_header = mipped.at("header").as_object();
	BOOST_CHECK_EQUAL(text_of(mip_header.at("wrap")), "repeat");
	BOOST_CHECK_EQUAL(text_of(mip_header.at("mag")), "linear");
	BOOST_CHECK_EQUAL(text_of(mip_header.at("min")), "linear_mipmap_nearest");
	BOOST_CHECK_EQUAL(mip_header.at("data").as_object().at("mip_levels").as_int64(), edt::k_image_full_mip_chain);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtImageImportTests, image_fixture)

BOOST_AUTO_TEST_CASE(AnImport_WritesTheSidecarThePictureAndTheTextureDesc)
{
	const json::object done = imported(request(json::parse(R"({"wrap":"repeat","filter":"linear","mips":true})")));
	BOOST_CHECK_EQUAL(text_of(done.at("image")), "res://textures/rock.png");
	BOOST_CHECK_EQUAL(text_of(done.at("resource")), "res://textures/rock.png.resource");
	BOOST_CHECK_EQUAL(text_of(done.at("texture")), "res://textures/rock.png.txm.desc");

	// The sidecar: importer texture, the settings, canonical bytes.
	const std::string meta_text = read_file(top() / "textures" / "rock.png.resource");
	const res::asset_meta_result meta = res::read_asset_meta(meta_text);
	BOOST_REQUIRE(meta);
	BOOST_CHECK_EQUAL(meta.meta->importer, "texture");
	BOOST_CHECK(meta.meta->flags.empty());
	BOOST_CHECK_EQUAL(res::to_text(meta.meta->guid), text_of(done.at("guid")));
	const json::object expected_settings{ { "version", 1 }, { "wrap", "repeat" }, { "filter", "linear" },
		{ "mips", true } };
	BOOST_CHECK_EQUAL(json::serialize(meta.meta->settings), json::serialize(expected_settings));
	BOOST_CHECK_EQUAL(res::write_asset_meta(*meta.meta), meta_text);

	// The bytes as they are.
	BOOST_CHECK(read_file(top() / "textures" / "rock.png") == png_text);

	// The texture desc names the picture and carries only the header the settings make.
	const json::object desc = json::parse(read_file(top() / "textures" / "rock.png.txm.desc")).as_object();
	BOOST_CHECK_EQUAL(text_of(desc.at("data")), "res://textures/rock.png");
	const json::object& header = desc.at("header").as_object();
	BOOST_CHECK_EQUAL(text_of(header.at("wrap")), "repeat");
	BOOST_CHECK_EQUAL(text_of(header.at("mag")), "linear");
	BOOST_CHECK_EQUAL(text_of(header.at("min")), "linear_mipmap_nearest");
	BOOST_CHECK(!header.contains("type"));
	BOOST_CHECK(!header.contains("usage"));
	BOOST_CHECK(!header.at("data").as_object().contains("extent"));
	BOOST_CHECK(!header.at("data").as_object().contains("format"));

	// Nothing in the lower layer; the main thread was told of the three files.
	BOOST_CHECK(fs::is_empty(lower()));
	BOOST_CHECK_EQUAL(reported.size(), 3u);
}

BOOST_AUTO_TEST_CASE(AFolderAndAName_AreTheOnesAsked_TheExtensionIsAdded)
{
	const json::object done = imported(request(nullptr, std::string{ "res://materials/stone" }, "granite"));
	BOOST_CHECK_EQUAL(text_of(done.at("image")), "res://materials/stone/granite.png");
	BOOST_CHECK(fs::exists(top() / "materials" / "stone" / "granite.png.txm.desc"));
}

BOOST_AUTO_TEST_CASE(TheAssetDatabase_KeepsTheSidecarsId_ReportedOrWalked)
{
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower(), top() });
	db->rebuild();
	test_util::pump(tasks);

	const json::object done = imported(request());
	test_util::pump(tasks);
	db->poll(true);
	test_util::pump(tasks);
	const std::string sidecar = read_file(top() / "textures" / "rock.png.resource");
	const edt::asset_record* record = db->find(res::tag::make("textures/rock.png"));
	BOOST_REQUIRE(record != nullptr);
	BOOST_REQUIRE(record->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*record->id), text_of(done.at("guid")));
	BOOST_CHECK_EQUAL(read_file(top() / "textures" / "rock.png.resource"), sidecar);

	// A database made afterwards reads the same ID from the walk.
	edt::asset_database walked(jobs, &events, std::vector<fs::path>{ lower(), top() });
	walked.rebuild();
	test_util::pump(tasks);
	const edt::asset_record* again = walked.find(res::tag::make("textures/rock.png"));
	BOOST_REQUIRE(again != nullptr && again->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*again->id), text_of(done.at("guid")));
}

BOOST_AUTO_TEST_CASE(ANameTakenInAnyLayer_IsRefused_ForEachOfTheThreeFiles)
{
	for (const char* taken : { "textures/rock.png", "textures/rock.png.resource", "textures/rock.png.txm.desc" }) {
		BOOST_TEST_CONTEXT(taken)
		{
			const fs::path holder = lower() / taken;
			fs::create_directories(holder.parent_path());
			std::ofstream(holder) << "{}";
			BOOST_CHECK_EQUAL(refusal_code(request()), cmd::codes::needs_input);
			const edt::image_importer::started started = importer->start(request(), cmd::origin::ui(), false);
			BOOST_CHECK(!started.ok());
			BOOST_CHECK_EQUAL(started.code, cmd::codes::needs_input);
			fs::remove(holder);
		}
	}
	// In the top layer too.
	dir.write("project/res/textures/ROCK.PNG.TXM.DESC", "{}");
	BOOST_CHECK_EQUAL(refusal_code(request()), cmd::codes::needs_input);
}

BOOST_AUTO_TEST_CASE(ALeftoverSidecar_HoldsTheName)
{
	dir.write("project/res/textures/rock.png.resource", "left over");
	const edt::image_import_plan plan = importer->resolve(request(), true);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::needs_input);
	BOOST_CHECK_MESSAGE(plan.error.find(".resource") != std::string::npos, plan.error);
}

BOOST_AUTO_TEST_CASE(WhatIsNoPictureOnDisk_IsRefused)
{
	dir.write("outside/notes.txt", "text");
	dir.write("outside/empty.png", "");
	dir.write("outside/big.png", std::string(5000, 'x'));
	fs::create_directories(dir.path() / "outside" / "folder.png");
	const auto code_of = [this](const fs::path& path, bool person) {
		edt::image_import_request wanted = request();
		wanted.path = path.string();
		const edt::image_import_plan plan = importer->resolve(wanted, person);
		BOOST_CHECK(!plan.ok());
		return plan.code;
	};
	BOOST_CHECK_EQUAL(code_of("rock.png", true), cmd::codes::invalid_args);	// not absolute
	BOOST_CHECK_EQUAL(code_of(dir.path() / "outside" / "missing.png", true), cmd::codes::not_found);
	BOOST_CHECK_EQUAL(code_of(dir.path() / "outside" / "notes.txt", true), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(code_of(dir.path() / "outside" / "empty.png", true), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(code_of(dir.path() / "outside" / "folder.png", true), cmd::codes::invalid_args);
	// Too large for an agent (the fixture's limit is 4 KiB); a person's limit is larger.
	BOOST_CHECK_EQUAL(code_of(dir.path() / "outside" / "big.png", false), cmd::codes::invalid_args);
	edt::image_import_request big = request();
	big.path = (dir.path() / "outside" / "big.png").string();
	BOOST_CHECK(importer->resolve(big, true).ok());
}

BOOST_AUTO_TEST_CASE(BytesThatAreNoPicture_FailTheJob_AndWriteNothing)
{
	for (const char* name : { "fake.png", "fake.jpg" }) {
		BOOST_TEST_CONTEXT(name)
		{
			edt::image_import_request wanted = request();
			wanted.path = dir.write(fs::path("outside") / name, "this is not a picture").string();
			const auto info = run(wanted);
			BOOST_REQUIRE(info.has_value());
			BOOST_CHECK(info->status == edt::job_status::failed);
			BOOST_CHECK(nothing_written());
		}
	}
	// A PNG named as a JPEG is no JPEG.
	edt::image_import_request renamed = request();
	renamed.path = dir.write("outside/rock.jpg", png_text).string();
	const auto info = run(renamed);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(nothing_written());
	BOOST_CHECK(reported.empty());
	const std::string problem = edt::image_importer::image_problem(std::as_bytes(std::span(png_text)), ".png");
	BOOST_CHECK_MESSAGE(problem.empty(), problem);
}

BOOST_AUTO_TEST_CASE(ALinkAsTheSource_IsRefused)
{
	std::error_code ec;
	const fs::path link = dir.path() / "outside" / "link.png";
	fs::create_symlink(source, link, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	edt::image_import_request wanted = request();
	wanted.path = link.string();
	BOOST_CHECK_EQUAL(refusal_code(wanted), cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(BadSettings_ABadNameOrFolder_AreRefused)
{
	BOOST_CHECK_EQUAL(refusal_code(request(json::parse(R"({"wrap":"repeat","srgb":true})"))), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(refusal_code(request(json::parse(R"({"mips":1})"))), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(refusal_code(request(nullptr, std::string{ "edt://themes" })), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(refusal_code(request(nullptr, std::string{ "../outside" })), cmd::codes::invalid_args);
	// Another format than the source's.
	BOOST_CHECK_EQUAL(refusal_code(request(nullptr, std::nullopt, "rock.jpg")), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(refusal_code(request(nullptr, std::nullopt, "a/b")), cmd::codes::invalid_args);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_CASE(ABlocker_OrARunningImport_IsBusy)
{
	blocked = "a project migration runs";
	BOOST_CHECK_EQUAL(refusal_code(request()), cmd::codes::busy);
	blocked.reset();

	const edt::image_importer::started first = importer->start(request(), cmd::origin::ui(), false);
	BOOST_REQUIRE(first.ok());
	BOOST_CHECK(importer->running());
	BOOST_CHECK_EQUAL(refusal_code(request(nullptr, std::string{ "other" })), cmd::codes::busy);
	test_util::pump(tasks);
	BOOST_CHECK(!importer->running());
}

BOOST_AUTO_TEST_CASE(ASecondOrThirdWriteThatFails_LeavesNothing)
{
	for (const char* step : { "image", "texture" }) {
		BOOST_TEST_CONTEXT(step)
		{
			failing_step = step;
			const auto info = run(request());
			BOOST_REQUIRE(info.has_value());
			BOOST_CHECK(info->status == edt::job_status::failed);
			BOOST_CHECK(nothing_written());
			BOOST_CHECK(reported.empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(ATargetThatAppearsBeforeTheWrite_IsNotOverwritten_AndTheRestIsTakenBack)
{
	const edt::image_import_plan plan = importer->resolve(request(), true);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	dir.write("project/res/textures/rock.png.txm.desc", "someone else's");
	const edt::image_import_written written =
		edt::image_importer::write_files(top(), plan, std::as_bytes(std::span(png_text)));
	BOOST_CHECK(!written.ok());
	BOOST_CHECK(!fs::exists(top() / "textures" / "rock.png.resource"));
	BOOST_CHECK(!fs::exists(top() / "textures" / "rock.png"));
	BOOST_CHECK_EQUAL(read_file(top() / "textures" / "rock.png.txm.desc"), "someone else's");
}

BOOST_AUTO_TEST_CASE(AnAgent_NeedsAPersonsApproval_OfThePlanShown)
{
	const edt::image_importer::started unapproved = importer->start(request(), agent, false);
	BOOST_CHECK_EQUAL(unapproved.code, cmd::codes::denied);
	const edt::image_importer::started not_shown = importer->start(request(), agent, true);
	BOOST_CHECK_EQUAL(not_shown.code, cmd::codes::denied);

	const std::optional<std::string> question = importer->describe(request(), agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK(question->find("res://textures/rock.png.txm.desc") != std::string::npos);
	BOOST_CHECK(question->find("rock.png") != std::string::npos);
	const edt::image_importer::started approved = importer->start(request(), agent, true);
	BOOST_REQUIRE_MESSAGE(approved.ok(), approved.error);
	test_util::pump(tasks);
	BOOST_CHECK(fs::exists(top() / "textures" / "rock.png.txm.desc"));
}

BOOST_AUTO_TEST_CASE(AnAgentsFileChangedAfterTheQuestion_IsDenied)
{
	BOOST_REQUIRE(importer->describe(request(), agent).has_value());
	{
		std::ofstream out(source, std::ios::binary | std::ios::app);
		out << "more";
	}
	const edt::image_importer::started started = importer->start(request(), agent, true);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtImageImportFormTests, image_fixture)

BOOST_AUTO_TEST_CASE(TheForm_StartsWithTheDefaultsAndTheSourcesName)
{
	edt::image_import_form form(*importer);
	form.open("art", source);
	BOOST_CHECK((form.settings() == edt::image_import_settings{}));
	BOOST_CHECK_EQUAL(form.name(), "rock.png");
	BOOST_CHECK_EQUAL(form.folder(), "art");
	BOOST_CHECK_MESSAGE(form.can_submit(), form.problem());
	const std::vector<std::string> names = form.final_names();
	BOOST_REQUIRE_EQUAL(names.size(), 3u);
	BOOST_CHECK_EQUAL(names[0], "res://art/rock.png");
	BOOST_CHECK_EQUAL(names[1], "res://art/rock.png.resource");
	BOOST_CHECK_EQUAL(names[2], "res://art/rock.png.txm.desc");
}

BOOST_AUTO_TEST_CASE(TheForm_ChangesSettingsAndNames_AndImports)
{
	edt::image_import_form form(*importer);
	form.open("", source);
	form.set_wrap(edt::image_wrap::repeat);
	form.set_filter(edt::image_filter::linear);
	form.set_mips(true);
	form.set_name("ground");
	BOOST_CHECK_EQUAL(form.final_names().at(0), "res://ground.png");

	dir.write("engine/taken.png", "x");
	form.set_name("taken.png");
	BOOST_CHECK(!form.can_submit());
	BOOST_CHECK(!form.problem().empty());
	BOOST_CHECK(form.final_names().empty());

	form.set_name("ground");
	const std::optional<std::uint64_t> job = form.submit();
	BOOST_REQUIRE_MESSAGE(job.has_value(), form.error());
	test_util::pump(tasks);
	const res::asset_meta_result meta = res::read_asset_meta(read_file(top() / "ground.png.resource"));
	BOOST_REQUIRE(meta);
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("wrap")), "repeat");
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("filter")), "linear");
	BOOST_CHECK(meta.meta->settings.at("mips").as_bool());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// file.import_image over the importer: what the registry and the policy see.
struct image_action_fixture : image_fixture
{
	image_action_fixture()
	{
		hooks.image_import = importer.get();
		menu = std::make_unique<edt::file_menu>(dialogs, actions, hooks);
		menu->register_actions();
	}

	~image_action_fixture() { menu.reset(); }

	cmd::result call(const cmd::origin& from, const json::object& args)
	{
		cmd::call request;
		request.args = args;
		request.from = from;
		return actions.invoke("file.import_image", request);
	}

	edt::dialog_service dialogs;
	edt::action_registry actions;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtImportImageActionTests, image_action_fixture)

BOOST_AUTO_TEST_CASE(TheCommand_AsksAPersonAndStartsAJob)
{
	const cmd::command_desc* command = actions.commands().find("file.import_image");
	BOOST_REQUIRE(command != nullptr);
	BOOST_CHECK((command->risk == cmd::danger::confirm));
	BOOST_CHECK(command->starts_job);
	BOOST_CHECK(command->mutates);
	BOOST_CHECK(static_cast<bool>(command->describe));
	BOOST_CHECK(!command->args_schema.as_object().at("additionalProperties").as_bool());
}

BOOST_AUTO_TEST_CASE(AnExtraProperty_IsRefusedByTheSchema)
{
	const cmd::result extra =
		call(cmd::origin::ui(), json::object{ { "path", source.string() }, { "overwrite", true } });
	BOOST_CHECK(!extra.ok);
	BOOST_CHECK_EQUAL(extra.code, cmd::codes::invalid_args);
	const cmd::result extra_setting = call(cmd::origin::ui(),
		json::object{ { "path", source.string() }, { "settings", json::object{ { "srgb", true } } } });
	BOOST_CHECK(!extra_setting.ok);
	BOOST_CHECK_EQUAL(extra_setting.code, cmd::codes::invalid_args);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_CASE(APersonsCall_StartsTheJob)
{
	const cmd::result result = call(cmd::origin::ui(), json::object{ { "path", source.string() }, { "folder", "art" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK_GT(result.value.as_object().at("job").as_int64(), 0);
	test_util::pump(tasks);
	BOOST_CHECK(fs::exists(top() / "art" / "rock.png.txm.desc"));
}

BOOST_AUTO_TEST_SUITE_END()
