#include <boost/test/unit_test.hpp>
#include "png_bomb_builder.hpp"
#include "test_folder_link.hpp"
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_file_menu.h"
#include "edt_image_import.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/command/cmd_policy.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// The import of a picture with its settings (#1193): four new files in the project's top layer (the picture's
// `.resource`, the texture desc's `.resource`, the bytes as they are, the texture desc), the asset database keeps both
// IDs, nothing stays when a write fails, and an agent's source is looked at only after approval and only on a local
// disk.

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
		result.project_open = [this] { return project_open; };
		result.drive_type = [this](char letter) {
			++drive_type_calls;
			return std::toupper(static_cast<unsigned char>(letter)) == remote_drive ? 4u : edt::k_drive_fixed;
		};
		result.mount_table = [this]() -> std::optional<std::string> {
			++mount_table_calls;
			return mounts;
		};
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
		result.rewritten = [this](const std::vector<res::tag>& tags) {
			rewritten = tags;
			if (db) {
				std::vector<res::file_change> changes;
				for (const res::tag& tag : tags) {
					res::file_change change;
					change.tag = tag;
					change.kind = res::change_kind::modified;
					changes.push_back(std::move(change));
				}
				db->report_self_change(changes);
			}
		};
		result.open_with_unsaved_changes = [this](const res::tag& tag) { return tag.view() == open_dirty; };
		result.now = [this] { return clock; };
		result.max_agent_bytes = 4096;
		result.fail_step = [this](std::string_view step) {
			if (on_step) on_step(step);
			return step == failing_step || step == failing_put_back;
		};
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
		const edt::image_importer::started started = importer->start(wanted, cmd::origin::ui());
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
	// Runs at each write step before it is failed or done: a test changes a file there.
	std::function<void(std::string_view)> on_step;
	std::vector<res::tag> reported;
	// A re-import's put-back that fails (#1194), and what a re-import reported as changed.
	std::string failing_put_back;
	std::vector<res::tag> rewritten;
	// The tag a document holds with unsaved edits; empty: none.
	std::string open_dirty;
	std::chrono::steady_clock::time_point clock{};
	const cmd::origin agent = cmd::origin::agent("session-1");
	bool project_open = true;
	// The classifier of an agent's source: every drive a fixed disk but this one, a local root file system.
	int remote_drive = 0;
	std::string mounts = "/dev/sda1 / ext4 rw 0 0\n";
	int drive_type_calls = 0;
	int mount_table_calls = 0;
};

// Another whole 2x2 RGBA picture: the new source of a re-import.
std::string other_png()
{
	const std::vector<unsigned char> rows{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 9, 10, 11, 12, 13, 14, 15, 16 };
	const std::vector<std::byte> bytes = test_util::png_bomb::png_rgba(2, 2, test_util::png_bomb::zlib_stream(rows, 0));
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A picture imported with the defaults (res://textures/rock.png) and a new source beside the old one (#1194).
template <class Base>
struct reimport_of : Base
{
	reimport_of()
	{
		first = this->imported(this->request());
		this->reported.clear();
		this->rewritten.clear();
		other = fs::weakly_canonical(this->dir.write("outside/new.png", other_text));
	}

	edt::image_reimport_request reimport(json::value settings = nullptr,
		std::string target = "res://textures/rock.png") const
	{
		edt::image_reimport_request out;
		out.target = std::move(target);
		out.path = other.string();
		out.settings = std::move(settings);
		return out;
	}

	std::optional<edt::job_service::job_info> run_reimport(const edt::image_reimport_request& wanted)
	{
		const edt::image_importer::started started = this->importer->start_reimport(wanted, cmd::origin::ui());
		BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
		test_util::pump(this->tasks);
		return this->jobs.info(started.job);
	}

	json::object reimported(const edt::image_reimport_request& wanted)
	{
		const auto info = run_reimport(wanted);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, info->error);
		BOOST_REQUIRE(info->result.has_value());
		return info->result->as_object();
	}

	std::string file(std::string_view relative) const { return read_file(this->top() / "textures" / relative); }

	// The four files as they are now.
	std::vector<std::string> files() const
	{
		return { file("rock.png"), file("rock.png.resource"), file("rock.png.txm.desc"),
			file("rock.png.txm.desc.resource") };
	}

	// A person's call that is refused before anything is written: its code.
	std::string refused(const edt::image_reimport_request& wanted, const cmd::origin& from = cmd::origin::ui())
	{
		const std::vector<std::string> before = files();
		const edt::image_importer::started started = this->importer->start_reimport(wanted, from);
		BOOST_CHECK(!started.ok());
		BOOST_CHECK_EQUAL(started.job, 0u);
		last_error = started.error;
		test_util::pump(this->tasks);
		BOOST_CHECK(files() == before);
		BOOST_CHECK(this->rewritten.empty());
		return started.code;
	}

	void rewrite_meta(const std::function<void(json::object&)>& change)
	{
		json::object root = json::parse(file("rock.png.resource")).as_object();
		change(root);
		this->dir.write("project/res/textures/rock.png.resource", json::serialize(root));
	}

	const std::string other_text = other_png();
	fs::path other;
	json::object first;
	std::string last_error;
};

using reimport_fixture = reimport_of<image_fixture>;

// A path on a network volume as the classifier is faked for this platform.
std::string remote_volume_path(image_fixture& fixture)
{
#if defined(_WIN32)
	fixture.remote_drive = 'Z';
	return "Z:\\pictures\\rock.png";
#else
	fixture.mounts += "nas:/export /mnt/nas nfs4 rw 0 0\n";
	return "/mnt/nas/pictures/rock.png";
#endif
}

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

BOOST_AUTO_TEST_CASE(AnImport_WritesBothSidecarsThePictureAndTheTextureDesc)
{
	const json::object done = imported(request(json::parse(R"({"wrap":"repeat","filter":"linear","mips":true})")));
	BOOST_CHECK_EQUAL(text_of(done.at("image")), "res://textures/rock.png");
	BOOST_CHECK_EQUAL(text_of(done.at("resource")), "res://textures/rock.png.resource");
	BOOST_CHECK_EQUAL(text_of(done.at("texture")), "res://textures/rock.png.txm.desc");
	BOOST_CHECK_EQUAL(text_of(done.at("texture_resource")), "res://textures/rock.png.txm.desc.resource");
	BOOST_CHECK_NE(text_of(done.at("guid")), text_of(done.at("texture_guid")));

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

	// The desc's own sidecar: its ID, no importer, no settings, canonical bytes.
	const std::string desc_meta_text = read_file(top() / "textures" / "rock.png.txm.desc.resource");
	const res::asset_meta_result desc_meta = res::read_asset_meta(desc_meta_text);
	BOOST_REQUIRE(desc_meta);
	BOOST_CHECK(desc_meta.meta->importer.empty());
	BOOST_CHECK(desc_meta.meta->settings.empty());
	BOOST_CHECK(desc_meta.meta->flags.empty());
	BOOST_CHECK_EQUAL(res::to_text(desc_meta.meta->guid), text_of(done.at("texture_guid")));
	BOOST_CHECK_EQUAL(res::write_asset_meta(*desc_meta.meta), desc_meta_text);

	// Nothing in the lower layer; the main thread was told of the four files, in the order they were written.
	BOOST_CHECK(fs::is_empty(lower()));
	BOOST_REQUIRE_EQUAL(reported.size(), 4u);
	BOOST_CHECK_EQUAL(reported[0].view(), "res://textures/rock.png.resource");
	BOOST_CHECK_EQUAL(reported[1].view(), "res://textures/rock.png.txm.desc.resource");
	BOOST_CHECK_EQUAL(reported[2].view(), "res://textures/rock.png");
	BOOST_CHECK_EQUAL(reported[3].view(), "res://textures/rock.png.txm.desc");
}

BOOST_AUTO_TEST_CASE(AFolderAndAName_AreTheOnesAsked_TheExtensionIsAdded)
{
	const json::object done = imported(request(nullptr, std::string{ "res://materials/stone" }, "granite"));
	BOOST_CHECK_EQUAL(text_of(done.at("image")), "res://materials/stone/granite.png");
	BOOST_CHECK(fs::exists(top() / "materials" / "stone" / "granite.png.txm.desc"));
}

BOOST_AUTO_TEST_CASE(TheAssetDatabase_KeepsBothSidecarsIds_ReportedOrWalked)
{
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower(), top() });
	db->rebuild();
	test_util::pump(tasks);

	const json::object done = imported(request());
	test_util::pump(tasks);
	db->poll(true);
	test_util::pump(tasks);
	const std::string sidecar = read_file(top() / "textures" / "rock.png.resource");
	const std::string desc_sidecar = read_file(top() / "textures" / "rock.png.txm.desc.resource");
	const edt::asset_record* record = db->find(res::tag::make("textures/rock.png"));
	BOOST_REQUIRE(record != nullptr);
	BOOST_REQUIRE(record->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*record->id), text_of(done.at("guid")));
	const edt::asset_record* desc = db->find(res::tag::make("textures/rock.png.txm.desc"));
	BOOST_REQUIRE(desc != nullptr);
	BOOST_REQUIRE(desc->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*desc->id), text_of(done.at("texture_guid")));
	BOOST_CHECK_EQUAL(read_file(top() / "textures" / "rock.png.resource"), sidecar);
	BOOST_CHECK_EQUAL(read_file(top() / "textures" / "rock.png.txm.desc.resource"), desc_sidecar);

	// A database made afterwards reads the same ID from the walk.
	edt::asset_database walked(jobs, &events, std::vector<fs::path>{ lower(), top() });
	walked.rebuild();
	test_util::pump(tasks);
	const edt::asset_record* again = walked.find(res::tag::make("textures/rock.png"));
	BOOST_REQUIRE(again != nullptr && again->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*again->id), text_of(done.at("guid")));
	const edt::asset_record* desc_again = walked.find(res::tag::make("textures/rock.png.txm.desc"));
	BOOST_REQUIRE(desc_again != nullptr && desc_again->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*desc_again->id), text_of(done.at("texture_guid")));
}

BOOST_AUTO_TEST_CASE(ANameTakenInAnyLayer_IsRefused_ForEachOfTheFourFiles)
{
	for (const char* taken : { "textures/rock.png", "textures/rock.png.resource", "textures/rock.png.txm.desc",
		"textures/rock.png.txm.desc.resource" }) {
		BOOST_TEST_CONTEXT(taken)
		{
			const fs::path holder = lower() / taken;
			fs::create_directories(holder.parent_path());
			std::ofstream(holder) << "{}";
			BOOST_CHECK_EQUAL(refusal_code(request()), cmd::codes::needs_input);
			const edt::image_importer::started started = importer->start(request(), cmd::origin::ui());
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

// #1408: the same walk as the model import's: a link on the way to an agent's picture is refused where it is.
BOOST_AUTO_TEST_CASE(AnAgentsSource_BelowAFolderLink_IsRefusedAtTheLink)
{
	const fs::path outside = source.parent_path();
	const fs::path pictures = fs::weakly_canonical(dir.write("pictures/rock.png", png_text)).parent_path();
	test_util::make_folder_link(outside / "linked", pictures);
	edt::image_import_request wanted = request();
	wanted.path = (outside / "linked" / "rock.png").string();
	const edt::image_import_plan plan = importer->resolve(wanted, false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(plan.error.find("'linked' on the way to the source is a link") != std::string::npos, plan.error);
	// A person's pick goes through the link to the file, as before.
	BOOST_CHECK(importer->resolve(wanted, true).ok());
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

	const edt::image_importer::started first = importer->start(request(), cmd::origin::ui());
	BOOST_REQUIRE(first.ok());
	BOOST_CHECK(importer->running());
	BOOST_CHECK_EQUAL(refusal_code(request(nullptr, std::string{ "other" })), cmd::codes::busy);
	test_util::pump(tasks);
	BOOST_CHECK(!importer->running());
}

BOOST_AUTO_TEST_CASE(ASecondThirdOrFourthWriteThatFails_LeavesNothing)
{
	for (const char* step : { "texture_meta", "image", "texture" }) {
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
	BOOST_CHECK(!fs::exists(top() / "textures" / "rock.png.txm.desc.resource"));
	BOOST_CHECK(!fs::exists(top() / "textures" / "rock.png"));
	BOOST_CHECK_EQUAL(read_file(top() / "textures" / "rock.png.txm.desc"), "someone else's");
	// The error names the file as res:// and holds nothing of this machine's folders.
	BOOST_CHECK_MESSAGE(written.error.find("unit_test_tmp") == std::string::npos, written.error);
	BOOST_CHECK_MESSAGE(written.error.find(dir.path().generic_string()) == std::string::npos, written.error);
}

BOOST_AUTO_TEST_CASE(AFailingWritesError_HoldsNoAbsolutePath)
{
	const edt::image_import_plan plan = importer->resolve(request(), true);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	// A file where the folder must be made: the folder cannot be made.
	dir.write("project/res/textures", "a file");
	const edt::image_import_written written =
		edt::image_importer::write_files(top(), plan, std::as_bytes(std::span(png_text)));
	BOOST_REQUIRE(!written.ok());
	for (const std::string& part : { std::string("unit_test_tmp"), dir.path().string(), dir.path().generic_string(),
		top().string(), top().generic_string() }) {
		BOOST_CHECK_MESSAGE(written.error.find(part) == std::string::npos, written.error);
	}

}

BOOST_AUTO_TEST_CASE(WithNoProjectOpen_NothingIsImported)
{
	project_open = false;
	BOOST_CHECK_EQUAL(refusal_code(request()), cmd::codes::disabled);
	const edt::image_importer::started person = importer->start(request(), cmd::origin::ui());
	BOOST_CHECK_EQUAL(person.code, cmd::codes::disabled);
	const std::optional<std::string> question = importer->describe(request(), agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK_MESSAGE(question->find("no project is open") != std::string::npos, *question);
	edt::image_import_form form(*importer);
	form.open("art", source);
	BOOST_CHECK(!form.can_submit());
	BOOST_CHECK(fs::is_empty(lower()));
	BOOST_CHECK(fs::is_empty(top()));
}

BOOST_AUTO_TEST_CASE(AnAgent_NeedsThePlanDescribedForThatVeryCall)
{
	const edt::image_importer::started never_described = importer->start(request(), agent);
	BOOST_CHECK_EQUAL(never_described.code, cmd::codes::denied);

	const std::optional<std::string> question = importer->describe(request(), agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK(question->find("res://textures/rock.png.txm.desc") != std::string::npos);
	BOOST_CHECK(question->find("res://textures/rock.png.txm.desc.resource") != std::string::npos);
	BOOST_CHECK(question->find("rock.png") != std::string::npos);
	// Another call of the same session was not described.
	BOOST_CHECK_EQUAL(importer->start(request(nullptr, std::string{ "other" }), agent).code, cmd::codes::denied);
	const edt::image_importer::started described = importer->start(request(), agent);
	BOOST_REQUIRE_MESSAGE(described.ok(), described.error);
	test_util::pump(tasks);
	BOOST_CHECK(fs::exists(top() / "textures" / "rock.png.txm.desc"));
	// The plan is spent.
	BOOST_CHECK_EQUAL(importer->start(request(nullptr, std::nullopt, "again"), agent).code, cmd::codes::denied);
}

BOOST_AUTO_TEST_CASE(AnAgent_RunsOnlyThePlanOfItsOwnTicket)
{
	// Described for one question: a call without that ticket, or with another, uses the plan up and runs nothing.
	BOOST_REQUIRE(importer->describe(request(), agent, "ticket-a").has_value());
	BOOST_CHECK_EQUAL(importer->start(request(), agent).code, cmd::codes::denied);
	BOOST_REQUIRE(importer->describe(request(), agent, "ticket-a").has_value());
	BOOST_CHECK_EQUAL(importer->start(request(), agent, "ticket-b").code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(importer->start(request(), agent, "ticket-a").code, cmd::codes::denied);
	test_util::pump(tasks);
	BOOST_CHECK(nothing_written());

	BOOST_REQUIRE(importer->describe(request(), agent, "ticket-a").has_value());
	const edt::image_importer::started started = importer->start(request(), agent, "ticket-a");
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	test_util::pump(tasks);
	BOOST_CHECK(fs::exists(top() / "textures" / "rock.png.txm.desc"));
}

BOOST_AUTO_TEST_CASE(AnAgentsQuestion_DoesNotLookAtTheSource)
{
	// A file that is not there: the question is made from the text, the start (after approval) finds it missing.
	edt::image_import_request missing = request();
	missing.path = (dir.path() / "outside" / "missing.png").string();
	const std::optional<std::string> question = importer->describe(missing, agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK_MESSAGE(question->find("This request will be refused") == std::string::npos, *question);
	BOOST_CHECK_MESSAGE(question->find("approved") != std::string::npos, *question);
	BOOST_CHECK_EQUAL(importer->start(missing, agent).code, cmd::codes::not_found);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_CASE(AnAgentsFileChangedAfterTheStart_FailsTheJob)
{
	BOOST_REQUIRE(importer->describe(request(), agent).has_value());
	const edt::image_importer::started started = importer->start(request(), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	{
		std::ofstream out(source, std::ios::binary | std::ios::app);
		out << "more";
	}
	test_util::pump(tasks);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(nothing_written());
}

// #1412: a plain file of the same size and content swapped in after the start is another file: the stamp of the handle
// the job reads from tells, not a look by the path.
BOOST_AUTO_TEST_CASE(AnAgentsFileSwappedForAnotherOfTheSameSize_FailsTheJob)
{
	BOOST_REQUIRE(importer->describe(request(), agent).has_value());
	const edt::image_importer::started started = importer->start(request(), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	std::string bytes;
	{
		std::ifstream in(source, std::ios::binary);
		bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	const fs::path other = source.parent_path() / "other.png";
	{
		std::ofstream out(other, std::ios::binary);
		out << bytes;
	}
	fs::rename(other, source);
	test_util::pump(tasks);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("changed since it was shown") != std::string::npos, info->error);
	BOOST_CHECK(nothing_written());
}

// #1412: the same size written in place with the write time set back: the change time of the handle tells.
BOOST_AUTO_TEST_CASE(AnAgentsFileRewrittenWithItsTimeSetBack_FailsTheJob)
{
	BOOST_REQUIRE(importer->describe(request(), agent).has_value());
	const edt::image_importer::started started = importer->start(request(), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	std::string bytes;
	{
		std::ifstream in(source, std::ios::binary);
		bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	BOOST_REQUIRE(!bytes.empty());
	bytes.back() = static_cast<char>(bytes.back() ^ 1);
	const fs::file_time_type written = fs::last_write_time(source);
	{
		std::ofstream out(source, std::ios::binary | std::ios::trunc);
		out << bytes;
	}
	fs::last_write_time(source, written);
	test_util::pump(tasks);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("changed since it was shown") != std::string::npos, info->error);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_CASE(AnAgentsNetworkSource_IsRefusedBeforeAnyDiskAccess)
{
	for (const std::string& path : { std::string("\\\\server\\share\\x.png"), std::string("//server/share/x.png"),
		std::string("\\\\?\\UNC\\server\\share\\x.png"), std::string("\\\\.\\C:\\x.png"),
		std::string("\\??\\C:\\x.png") }) {
		BOOST_TEST_CONTEXT(path)
		{
			edt::image_import_request wanted = request();
			wanted.path = path;
			const std::optional<std::string> question = importer->describe(wanted, agent);
			BOOST_REQUIRE(question.has_value());
			BOOST_CHECK_MESSAGE(question->find("network share or a device path") != std::string::npos, *question);
			const edt::image_import_plan plan = importer->resolve(wanted, false);
			BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
			BOOST_CHECK_MESSAGE(plan.error.find("no such file") == std::string::npos, plan.error);
			BOOST_CHECK_EQUAL(importer->start(wanted, agent).code, cmd::codes::denied);
		}
	}
	// Told from the text alone: neither the drive table nor the mount table was asked.
	BOOST_CHECK_EQUAL(drive_type_calls, 0);
	BOOST_CHECK_EQUAL(mount_table_calls, 0);

	// A mapped network drive (Windows) or a network mount (POSIX): the classifier says so, the path is not looked at.
	edt::image_import_request remote = request();
	remote.path = remote_volume_path(*this);
	const std::optional<std::string> question = importer->describe(remote, agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK_MESSAGE(question->find("This request will be refused") != std::string::npos, *question);
	BOOST_CHECK_MESSAGE(question->find("network") != std::string::npos, *question);
	const edt::image_import_plan plan = importer->resolve(remote, false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(plan.error.find("no such file") == std::string::npos, plan.error);
	BOOST_CHECK_GT(drive_type_calls + mount_table_calls, 0);
	// A person may pick a file on a share: the rule is not theirs (the file is simply missing here).
	BOOST_CHECK_EQUAL(importer->resolve(remote, true).code, cmd::codes::not_found);
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
	BOOST_REQUIRE_EQUAL(names.size(), 4u);
	BOOST_CHECK_EQUAL(names[0], "res://art/rock.png");
	BOOST_CHECK_EQUAL(names[1], "res://art/rock.png.resource");
	BOOST_CHECK_EQUAL(names[2], "res://art/rock.png.txm.desc");
	BOOST_CHECK_EQUAL(names[3], "res://art/rock.png.txm.desc.resource");
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

BOOST_FIXTURE_TEST_SUITE(EdtImageReimportTests, reimport_fixture)

BOOST_AUTO_TEST_CASE(AReimport_ReplacesTheBytes_BothSidecarsKeepTheirIds)
{
	const std::vector<std::string> before = files();
	const json::object done = reimported(reimport());
	BOOST_CHECK(file("rock.png") == other_text);
	BOOST_CHECK_EQUAL(file("rock.png.resource"), before[1]);
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc"), before[2]);
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc.resource"), before[3]);
	BOOST_CHECK_EQUAL(text_of(done.at("image")), "res://textures/rock.png");
	BOOST_CHECK_EQUAL(text_of(done.at("resource")), "res://textures/rock.png.resource");
	BOOST_CHECK_EQUAL(text_of(done.at("texture")), "res://textures/rock.png.txm.desc");
	BOOST_CHECK_EQUAL(text_of(done.at("guid")), text_of(first.at("guid")));
	BOOST_CHECK_EQUAL(text_of(done.at("texture_guid")), text_of(first.at("texture_guid")));
	BOOST_CHECK(!done.at("settings_changed").as_bool());
	BOOST_CHECK(!done.at("texture_rewritten").as_bool());
	// Only what was written is reported, as changed.
	BOOST_CHECK(reported.empty());
	BOOST_REQUIRE_EQUAL(rewritten.size(), 1u);
	BOOST_CHECK_EQUAL(rewritten[0].view(), "res://textures/rock.png");
	BOOST_CHECK(fs::is_empty(lower()));
}

BOOST_AUTO_TEST_CASE(TheAssetDatabase_KeepsTheIds_AcrossAReimport)
{
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower(), top() });
	db->rebuild();
	test_util::pump(tasks);
	reimported(reimport(json::parse(R"({"wrap":"repeat"})")));
	test_util::pump(tasks);
	db->poll(true);
	test_util::pump(tasks);
	const edt::asset_record* record = db->find(res::tag::make("textures/rock.png"));
	BOOST_REQUIRE(record != nullptr && record->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*record->id), text_of(first.at("guid")));
	const edt::asset_record* desc = db->find(res::tag::make("textures/rock.png.txm.desc"));
	BOOST_REQUIRE(desc != nullptr && desc->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*desc->id), text_of(first.at("texture_guid")));

	edt::asset_database walked(jobs, &events, std::vector<fs::path>{ lower(), top() });
	walked.rebuild();
	test_util::pump(tasks);
	const edt::asset_record* again = walked.find(res::tag::make("textures/rock.png"));
	BOOST_REQUIRE(again != nullptr && again->id.has_value());
	BOOST_CHECK_EQUAL(res::to_text(*again->id), text_of(first.at("guid")));
}

BOOST_AUTO_TEST_CASE(ChangedSettings_RewriteTheResourceAndTheTextureDesc_KeepTheGuid)
{
	const std::vector<std::string> before = files();
	const json::object done = reimported(reimport(json::parse(R"({"wrap":"repeat","filter":"linear","mips":true})")));
	BOOST_CHECK(done.at("settings_changed").as_bool());
	BOOST_CHECK(done.at("texture_rewritten").as_bool());
	const std::string meta_text = file("rock.png.resource");
	const res::asset_meta_result meta = res::read_asset_meta(meta_text);
	BOOST_REQUIRE(meta);
	BOOST_CHECK_EQUAL(res::to_text(meta.meta->guid), text_of(first.at("guid")));
	BOOST_CHECK_EQUAL(meta.meta->importer, "texture");
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("wrap")), "repeat");
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("filter")), "linear");
	BOOST_CHECK(meta.meta->settings.at("mips").as_bool());
	BOOST_CHECK_EQUAL(res::write_asset_meta(*meta.meta), meta_text);
	const json::object desc = json::parse(file("rock.png.txm.desc")).as_object();
	BOOST_CHECK_EQUAL(text_of(desc.at("header").as_object().at("wrap")), "repeat");
	BOOST_CHECK_EQUAL(text_of(desc.at("header").as_object().at("min")), "linear_mipmap_nearest");
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc.resource"), before[3]);
	// The recipe last.
	BOOST_REQUIRE_EQUAL(rewritten.size(), 3u);
	BOOST_CHECK_EQUAL(rewritten[0].view(), "res://textures/rock.png");
	BOOST_CHECK_EQUAL(rewritten[1].view(), "res://textures/rock.png.txm.desc");
	BOOST_CHECK_EQUAL(rewritten[2].view(), "res://textures/rock.png.resource");
}

BOOST_AUTO_TEST_CASE(NullSettings_KeepTheRecordedOnes_AndTheirFiles)
{
	reimported(reimport(json::parse(R"({"filter":"linear"})")));
	const std::vector<std::string> before = files();
	rewritten.clear();
	const json::object done = reimported(reimport());
	BOOST_CHECK(!done.at("settings_changed").as_bool());
	BOOST_CHECK(!done.at("texture_rewritten").as_bool());
	BOOST_CHECK_EQUAL(file("rock.png.resource"), before[1]);
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc"), before[2]);
	const res::asset_meta_result meta = res::read_asset_meta(file("rock.png.resource"));
	BOOST_REQUIRE(meta);
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("filter")), "linear");
	// The same settings given again are no change either.
	reimported(reimport(json::parse(R"({"version":1,"filter":"linear"})")));
	BOOST_CHECK_EQUAL(file("rock.png.resource"), before[1]);
}

BOOST_AUTO_TEST_CASE(AnyoneButAPerson_IsDenied)
{
	BOOST_CHECK_EQUAL(refused(reimport(), agent), cmd::codes::denied);
	BOOST_CHECK_MESSAGE(last_error.find("#1457") != std::string::npos, last_error);
	BOOST_CHECK_EQUAL(refused(reimport(), cmd::origin::test()), cmd::codes::denied);
	BOOST_CHECK_EQUAL(refused(reimport(), cmd::origin::game()), cmd::codes::denied);
	BOOST_CHECK(!importer->resolve_reimport(reimport(), agent).ok());
}

BOOST_AUTO_TEST_CASE(APictureWithoutItsResourceOrTextureDesc_IsRefused)
{
	const std::string meta_text = file("rock.png.resource");
	fs::remove(top() / "textures" / "rock.png.resource");
	BOOST_CHECK_EQUAL(importer->resolve_reimport(reimport(), cmd::origin::ui()).code, cmd::codes::not_found);
	BOOST_CHECK(file("rock.png") == png_text);
	dir.write("project/res/textures/rock.png.resource", meta_text);
	fs::remove(top() / "textures" / "rock.png.txm.desc");
	BOOST_CHECK_EQUAL(importer->resolve_reimport(reimport(), cmd::origin::ui()).code, cmd::codes::not_found);
	BOOST_CHECK(file("rock.png") == png_text);
}

BOOST_AUTO_TEST_CASE(AResourceNotInTheEditorsForm_IsRefused)
{
	dir.write("project/res/textures/rock.png.resource", file("rock.png.resource") + "\n");
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("not in the form") != std::string::npos, last_error);
}

BOOST_AUTO_TEST_CASE(AResourceOfANewerFormat_IsRefused)
{
	rewrite_meta([](json::object& root) { root["__version"] = 99; });
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("newer editor") != std::string::npos, last_error);
}

BOOST_AUTO_TEST_CASE(AResourceOfAnotherImporterOrWithUnknownSettings_IsRefused)
{
	const std::string kept = file("rock.png.resource");
	res::asset_meta meta = *res::read_asset_meta(kept).meta;
	meta.importer = "model";
	dir.write("project/res/textures/rock.png.resource", res::write_asset_meta(meta));
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("model") != std::string::npos, last_error);

	meta.importer = "texture";
	meta.settings["srgb"] = true;
	dir.write("project/res/textures/rock.png.resource", res::write_asset_meta(meta));
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("srgb") != std::string::npos, last_error);
}

BOOST_AUTO_TEST_CASE(APngTarget_TakesNoJpegSource)
{
	edt::image_reimport_request wanted = reimport();
	wanted.path = fs::weakly_canonical(dir.write("outside/new.jpg", "\xFF\xD8\xFF")).string();
	BOOST_CHECK_EQUAL(refused(wanted), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("PNG") != std::string::npos, last_error);
}

BOOST_AUTO_TEST_CASE(APictureOfALowerLayer_OrNoPicture_IsRefused)
{
	dir.write("engine/textures/old.png", png_text);
	BOOST_CHECK_EQUAL(refused(reimport(nullptr, "res://textures/old.png")), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("layer") != std::string::npos, last_error);
	BOOST_CHECK(read_file(lower() / "textures" / "old.png") == png_text);
	BOOST_CHECK_EQUAL(refused(reimport(nullptr, "res://textures/none.png")), cmd::codes::not_found);
	BOOST_CHECK_EQUAL(refused(reimport(nullptr, "res://textures/rock.png.txm.desc")), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(refused(reimport(nullptr, "engine://textures/rock.png")), cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(ABlocker_IsBusy)
{
	blocked = "Stop Play to import.";
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::busy);
}

BOOST_AUTO_TEST_CASE(APictureChangedBeforeItsWrite_IsNotOverwritten_AndNothingElseIsWritten)
{
	const std::vector<std::string> before = files();
	on_step = [this](std::string_view step) {
		if (step == "reimport_image") dir.write("project/res/textures/rock.png", "someone else's");
	};
	const auto info = run_reimport(reimport(json::parse(R"({"wrap":"repeat"})")));
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("changed since it was read") != std::string::npos, info->error);
	BOOST_CHECK_EQUAL(file("rock.png"), "someone else's");
	BOOST_CHECK_EQUAL(file("rock.png.resource"), before[1]);
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc"), before[2]);
	BOOST_CHECK(rewritten.empty());
}

BOOST_AUTO_TEST_CASE(AResourceChangedBeforeItsWrite_IsNotOverwritten_ThePictureAndDescArePutBack)
{
	const std::vector<std::string> before = files();
	const std::string theirs = before[1] + "\n";
	on_step = [this, &theirs](std::string_view step) {
		if (step == "reimport_meta") dir.write("project/res/textures/rock.png.resource", theirs);
	};
	const auto info = run_reimport(reimport(json::parse(R"({"wrap":"repeat"})")));
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("changed since it was read") != std::string::npos, info->error);
	BOOST_CHECK_EQUAL(file("rock.png.resource"), theirs);
	BOOST_CHECK(file("rock.png") == before[0]);
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc"), before[2]);
	BOOST_CHECK(rewritten.empty());
}

BOOST_AUTO_TEST_CASE(APictureChangedAfterTheStart_FailsTheJob)
{
	const edt::image_importer::started started = importer->start_reimport(reimport(), cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	dir.write("project/res/textures/rock.png", "someone else's");
	test_util::pump(tasks);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_EQUAL(file("rock.png"), "someone else's");
	BOOST_CHECK(rewritten.empty());
}

BOOST_AUTO_TEST_CASE(AFailureAtTheTextureOrResourceStep_PutsBackWhatWasReplaced)
{
	for (const char* step : { "reimport_texture", "reimport_meta" }) {
		BOOST_TEST_CONTEXT(step)
		{
			const std::vector<std::string> before = files();
			failing_step = step;
			const auto info = run_reimport(reimport(json::parse(R"({"mips":true})")));
			BOOST_REQUIRE(info.has_value());
			BOOST_CHECK(info->status == edt::job_status::failed);
			BOOST_CHECK(files() == before);
			BOOST_CHECK(rewritten.empty());
			BOOST_CHECK_MESSAGE(info->error.find("nothing was re-imported") != std::string::npos, info->error);
			BOOST_CHECK_MESSAGE(info->error.find(dir.path().generic_string()) == std::string::npos, info->error);
		}
	}
}

BOOST_AUTO_TEST_CASE(APutBackThatFails_SaysWhatStaysChanged_AndReportsIt)
{
	const std::vector<std::string> before = files();
	failing_step = "reimport_meta";
	failing_put_back = "put_back:reimport_image";
	const auto info = run_reimport(reimport(json::parse(R"({"wrap":"repeat"})")));
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("res://textures/rock.png stays changed") != std::string::npos, info->error);
	BOOST_CHECK_MESSAGE(info->error.find("nothing was re-imported") == std::string::npos, info->error);
	// The desc was put back, the picture could not be: the cache is told of it.
	BOOST_CHECK(file("rock.png") == other_text);
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc"), before[2]);
	BOOST_CHECK_EQUAL(file("rock.png.resource"), before[1]);
	BOOST_REQUIRE_EQUAL(rewritten.size(), 1u);
	BOOST_CHECK_EQUAL(rewritten[0].view(), "res://textures/rock.png");
}

BOOST_AUTO_TEST_CASE(AnEditedDesc_KeepsWhatWasEdited_OnlyTheSettingsFieldsChange)
{
	json::object desc = json::parse(file("rock.png.txm.desc")).as_object();
	desc["name"] = "custom name";
	desc["header"].as_object()["extra"] = "kept";
	const std::vector<std::byte> edited = res::pretty_json_bytes(desc);
	const std::string edited_text(reinterpret_cast<const char*>(edited.data()), edited.size());
	dir.write("project/res/textures/rock.png.txm.desc", edited_text);

	const json::object same = reimported(reimport());
	BOOST_CHECK(!same.at("texture_rewritten").as_bool());
	BOOST_CHECK_EQUAL(file("rock.png.txm.desc"), edited_text);

	const json::object changed = reimported(reimport(json::parse(R"({"wrap":"repeat","filter":"linear","mips":true})")));
	BOOST_CHECK(changed.at("texture_rewritten").as_bool());
	const json::object now = json::parse(file("rock.png.txm.desc")).as_object();
	BOOST_CHECK_EQUAL(text_of(now.at("name")), "custom name");
	BOOST_CHECK_EQUAL(text_of(now.at("data")), "res://textures/rock.png");
	const json::object& header = now.at("header").as_object();
	BOOST_CHECK_EQUAL(text_of(header.at("extra")), "kept");
	BOOST_CHECK_EQUAL(text_of(header.at("wrap")), "repeat");
	BOOST_CHECK_EQUAL(text_of(header.at("mag")), "linear");
	BOOST_CHECK_EQUAL(text_of(header.at("min")), "linear_mipmap_nearest");
	BOOST_CHECK_EQUAL(header.at("data").as_object().at("mip_levels").as_int64(), 32);
}

BOOST_AUTO_TEST_CASE(ADescOfAnotherPicture_OrNoDesc_IsRefused)
{
	json::object desc = json::parse(file("rock.png.txm.desc")).as_object();
	desc["data"] = "res://textures/other.png";
	dir.write("project/res/textures/rock.png.txm.desc", json::serialize(desc));
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(last_error.find("refers to another picture") != std::string::npos, last_error);
	dir.write("project/res/textures/rock.png.txm.desc", "not json");
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(ADescWithoutAHeaderToChange_IsRefusedOnlyWhenSettingsChange)
{
	json::object desc = json::parse(file("rock.png.txm.desc")).as_object();
	desc.erase("header");
	dir.write("project/res/textures/rock.png.txm.desc", json::serialize(desc));
	BOOST_CHECK_EQUAL(refused(reimport(json::parse(R"({"mips":true})"))), cmd::codes::invalid_args);
	BOOST_CHECK(importer->resolve_reimport(reimport(), cmd::origin::ui()).ok());
}

BOOST_AUTO_TEST_CASE(TheDescsOwnResource_MustBeThereAndTheEditors)
{
	const std::string kept = file("rock.png.txm.desc.resource");
	dir.write("project/res/textures/rock.png.txm.desc.resource", kept + "\n");
	BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::invalid_args);
	fs::remove(top() / "textures" / "rock.png.txm.desc.resource");
	BOOST_CHECK_EQUAL(importer->resolve_reimport(reimport(), cmd::origin::ui()).code, cmd::codes::not_found);
	BOOST_CHECK(file("rock.png") == png_text);
}

BOOST_AUTO_TEST_CASE(PartialSettings_KeepTheRecordedValues)
{
	reimported(reimport(json::parse(R"({"wrap":"repeat_mirror","filter":"linear"})")));
	reimported(reimport(json::parse(R"({"mips":true})")));
	const res::asset_meta_result meta = res::read_asset_meta(file("rock.png.resource"));
	BOOST_REQUIRE(meta);
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("wrap")), "repeat_mirror");
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("filter")), "linear");
	BOOST_CHECK(meta.meta->settings.at("mips").as_bool());
}

BOOST_AUTO_TEST_CASE(ALinkAtThePictureItsResourceOrItsDesc_IsNotFollowed)
{
	for (const char* name : { "rock.png", "rock.png.resource", "rock.png.txm.desc" }) {
		BOOST_TEST_CONTEXT(name)
		{
			const fs::path at = top() / "textures" / name;
			const fs::path real = dir.path() / "outside" / (std::string("real_") + name);
			fs::rename(at, real);
			std::error_code ec;
			fs::create_symlink(real, at, ec);
			if (ec) {
				BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
				fs::rename(real, at);
				return;
			}
			const std::string real_bytes = read_file(real);
			BOOST_CHECK_EQUAL(refused(reimport(json::parse(R"({"wrap":"repeat"})"))), cmd::codes::invalid_args);
			BOOST_CHECK(fs::is_symlink(at));
			BOOST_CHECK(read_file(real) == real_bytes);
			fs::remove(at);
			fs::rename(real, at);
		}
	}
}

BOOST_AUTO_TEST_CASE(TheSourceIsThePictureItself_IsRefused)
{
	edt::image_reimport_request wanted = reimport();
	wanted.path = (top() / "textures" / "rock.png").string();
	BOOST_CHECK_EQUAL(refused(wanted), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(last_error, "the source is the picture itself");
}

BOOST_AUTO_TEST_CASE(AFileOpenWithUnsavedChanges_IsBusy)
{
	for (const char* tag : { "res://textures/rock.png", "res://textures/rock.png.txm.desc" }) {
		open_dirty = tag;
		BOOST_CHECK_EQUAL(refused(reimport()), cmd::codes::busy);
		BOOST_CHECK_MESSAGE(last_error.find("unsaved changes") != std::string::npos, last_error);
	}
	open_dirty.clear();
	reimported(reimport());
}

BOOST_AUTO_TEST_CASE(ACancelledJob_WritesNothing)
{
	const std::vector<std::string> before = files();
	const edt::image_importer::started started = importer->start_reimport(reimport(json::parse(R"({"mips":true})")),
		cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	static_cast<void>(jobs.cancel(started.job));
	test_util::pump(tasks);
	BOOST_CHECK(files() == before);
	BOOST_CHECK(rewritten.empty());
	BOOST_CHECK(!importer->running());
}

BOOST_AUTO_TEST_CASE(BytesThatAreNoPicture_WriteNothing)
{
	const std::vector<std::string> before = files();
	dir.write("outside/new.png", "not a picture");
	const auto info = run_reimport(reimport());
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(files() == before);
}

BOOST_AUTO_TEST_CASE(TheForm_InReimportMode_StartsFromTheRecordedSettings_AndReimports)
{
	reimported(reimport(json::parse(R"({"wrap":"repeat_mirror"})")));
	edt::image_import_form form(*importer);
	form.open_reimport("res://textures/rock.png", other);
	BOOST_CHECK(form.reimport());
	BOOST_CHECK_EQUAL(form.target(), "res://textures/rock.png");
	BOOST_CHECK((form.settings().wrap == edt::image_wrap::repeat_mirror));
	BOOST_CHECK_MESSAGE(form.can_submit(), form.problem());
	BOOST_CHECK((form.rewritten_names() == std::vector<std::string>{ "res://textures/rock.png" }));
	form.set_mips(true);
	BOOST_CHECK((form.rewritten_names() == std::vector<std::string>{ "res://textures/rock.png",
		"res://textures/rock.png.txm.desc", "res://textures/rock.png.resource" }));
	const std::optional<std::uint64_t> job = form.submit();
	BOOST_REQUIRE_MESSAGE(job.has_value(), form.error());
	test_util::pump(tasks);
	const res::asset_meta_result meta = res::read_asset_meta(file("rock.png.resource"));
	BOOST_REQUIRE(meta);
	BOOST_CHECK(meta.meta->settings.at("mips").as_bool());
	BOOST_CHECK_EQUAL(text_of(meta.meta->settings.at("wrap")), "repeat_mirror");
	BOOST_CHECK_EQUAL(res::to_text(meta.meta->guid), text_of(first.at("guid")));

	form.open_reimport("res://textures/none.png", other);
	BOOST_CHECK(!form.can_submit());
	BOOST_CHECK(!form.problem().empty());
	form.open("art", source);
	BOOST_CHECK(!form.reimport());
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

	cmd::result call(const cmd::origin& from, const json::object& args, std::string ticket = {})
	{
		cmd::call request;
		request.args = args;
		request.from = from;
		request.confirmation = std::move(ticket);
		return actions.invoke("file.import_image", request);
	}

	edt::dialog_service dialogs;
	edt::action_registry actions;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
};

// A policy that says the same to every call, and a person who can be asked.
struct fixed_gate final : cmd::policy_gate
{
	explicit fixed_gate(cmd::policy_mode answer) : mode(answer) {}
	cmd::policy_decision decide(const cmd::command_desc&, const cmd::call&) override
	{
		return { mode, "test policy", cmd::agent_category::unset };
	}
	bool can_ask() const override { return true; }
	cmd::policy_mode mode;
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
	BOOST_CHECK(command->describe_before_allowed_invoke);
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

BOOST_AUTO_TEST_CASE(AnAgentsCallThePolicyAllows_IsDescribedByTheRegistry_AndRuns)
{
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::allow));
	const cmd::result result = call(agent, json::object{ { "path", source.string() } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	test_util::pump(tasks);
	BOOST_CHECK(fs::exists(top() / "textures" / "rock.png.txm.desc"));
	BOOST_CHECK(fs::exists(top() / "textures" / "rock.png.txm.desc.resource"));
	actions.commands().set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AnAgentsCall_WithATicketNotApprovedOrRejected_IsDenied_AnApprovedOneRuns)
{
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::ask));
	const json::object args{ { "path", source.string() } };
	const cmd::result asked = call(agent, args);
	BOOST_REQUIRE_EQUAL(asked.code, cmd::codes::confirmation_required);
	const std::string ticket(asked.value.at("ticket").as_string());
	BOOST_CHECK_EQUAL(call(agent, args, ticket).code, cmd::codes::denied);

	actions.commands().discard(ticket);
	BOOST_CHECK_EQUAL(call(agent, args, ticket).code, cmd::codes::denied);
	test_util::pump(tasks);
	BOOST_CHECK(nothing_written());

	const cmd::result again = call(agent, args);
	BOOST_REQUIRE_EQUAL(again.code, cmd::codes::confirmation_required);
	const std::string approved(again.value.at("ticket").as_string());
	BOOST_REQUIRE(actions.commands().approve(approved));
	const cmd::result ran = call(agent, args, approved);
	BOOST_REQUIRE_MESSAGE(ran.ok, ran.error);
	test_util::pump(tasks);
	BOOST_CHECK(fs::exists(top() / "textures" / "rock.png.txm.desc"));
	actions.commands().set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(APlanDescribedForARejectedTicket_RunsForNoOtherCall)
{
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::ask));
	const json::object args{ { "path", source.string() } };
	const cmd::result asked = call(agent, args);
	BOOST_REQUIRE_EQUAL(asked.code, cmd::codes::confirmation_required);
	actions.commands().discard(std::string(asked.value.at("ticket").as_string()));
	// No gate judges these calls: the importer alone holds the line.
	actions.commands().set_policy_gate(nullptr);
	BOOST_CHECK_EQUAL(call(agent, args).code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(call(agent, args, "another-ticket").code, cmd::codes::denied);
	test_util::pump(tasks);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_CASE(ACallNeverDescribed_IsDenied)
{
	// No gate judges a test's call and nothing describes it: the import refuses it.
	const cmd::result result = call(cmd::origin::test(), json::object{ { "path", source.string() } });
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, cmd::codes::denied);
	BOOST_CHECK(nothing_written());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

using reimport_action_fixture = reimport_of<image_action_fixture>;

}

BOOST_FIXTURE_TEST_SUITE(EdtReimportImageActionTests, reimport_action_fixture)

BOOST_AUTO_TEST_CASE(TheCommand_IsRegisteredWithItsSchema)
{
	const cmd::command_desc* command = actions.commands().find("file.reimport_image");
	BOOST_REQUIRE(command != nullptr);
	BOOST_CHECK(command->mutates);
	BOOST_CHECK(command->starts_job);
	BOOST_CHECK(!command->args_schema.as_object().at("additionalProperties").as_bool());
	cmd::call extra;
	extra.args = json::object{ { "target", "res://textures/rock.png" }, { "path", other.string() },
		{ "overwrite", true } };
	extra.from = cmd::origin::ui();
	BOOST_CHECK_EQUAL(actions.invoke("file.reimport_image", extra).code, cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(APersonsCall_Runs_AnAgentsIsDenied)
{
	cmd::call request;
	request.args = json::object{ { "target", "res://textures/rock.png" }, { "path", other.string() } };
	request.from = agent;
	const cmd::result denied = actions.invoke("file.reimport_image", request);
	BOOST_CHECK(!denied.ok);
	BOOST_CHECK_EQUAL(denied.code, cmd::codes::denied);
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::allow));
	BOOST_CHECK_EQUAL(actions.invoke("file.reimport_image", request).code, cmd::codes::denied);
	actions.commands().set_policy_gate(nullptr);
	test_util::pump(tasks);
	BOOST_CHECK(file("rock.png") == png_text);

	request.from = cmd::origin::ui();
	const cmd::result ran = actions.invoke("file.reimport_image", request);
	BOOST_REQUIRE_MESSAGE(ran.ok, ran.error);
	BOOST_CHECK_GT(ran.value.as_object().at("job").as_int64(), 0);
	test_util::pump(tasks);
	BOOST_CHECK(file("rock.png") == other_text);
}

BOOST_AUTO_TEST_SUITE_END()
