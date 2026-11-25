#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_shader_source_document.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The shader source document (#1202): the text of a res:// GLSL file, its dirty mark, the save and the conflicts with the disk.
namespace {

namespace fs = std::filesystem;

constexpr const char* k_frag = "#version 450 core\nout vec4 colour;\nvoid main()\n{\n\tcolour = vec4(1.0);\n}\n";

struct shader_document_fixture : test_util::scene_env
{
	shader_document_fixture()
	{
		fs::create_directories(res_root / "shaders");
		fs::create_directories(lower / "shaders");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		write(res_root, "shaders/scene.frag", k_frag);
		document = std::make_unique<edt::shader_source_document>(services);
		document->assign_id(11);
		document->set_files(layout());
		document->bind_events(&events);
	}

	~shader_document_fixture()
	{
		document.reset();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		out.write_blocker = [this] { return blocked; };
		return out;
	}

	static void write(const fs::path& root, std::string_view name, std::string_view content)
	{
		fs::create_directories((root / std::string(name)).parent_path());
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
	}

	static std::string text_of(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	void open_frag() { document->open(res::tag{ "res://shaders/scene.frag" }); }

	test_util::temp_dir dir{ "edt_shader_document" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	ecs::system_factory sf;
	edt::editor_services services;
	edt::editor_events events;
	std::vector<std::pair<edt::document_id, std::uint64_t>> changed;
	edt::connection seen = events.document_changed.subscribe([this](edt::document_id id, std::uint64_t generation) { changed.push_back({ id, generation }); });
	std::unique_ptr<edt::shader_source_document> document;
	std::optional<std::string> blocked;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtShaderSourceDocument, shader_document_fixture)

BOOST_AUTO_TEST_CASE(Open_AShaderSource_HoldsItsText_AndIsClean)
{
	open_frag();

	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_TEST(document->kind() == "shader_source");
	BOOST_TEST(document->title() == "scene.frag");
	BOOST_TEST(document->text() == k_frag);
	BOOST_TEST(document->line_count() == 7);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(document->can_save());
	BOOST_TEST(document->read_only_reason().empty());
}

BOOST_AUTO_TEST_CASE(Open_WhatIsNoShaderSource_IsRefusedAndSaysWhy)
{
	write(res_root, "shaders/scene.shader.desc", "{}");
	write(res_root, "shaders/binary.frag", std::string("a\0b", 3));
	write(res_root, "shaders/latin.frag", "caf\xE9");

	for (const char* tag : { "res://shaders/scene.shader.desc", "res://shaders/nothing.frag", "res://shaders/../x.frag", "edt://shaders/scene.frag",
		"res://shaders/binary.frag", "res://shaders/latin.frag" }) {
		document->open(res::tag{ tag });
		BOOST_TEST_MESSAGE(tag);
		BOOST_TEST(!document->is_open());
		BOOST_TEST(!document->last_error().empty());
	}
}

BOOST_AUTO_TEST_CASE(SetText_MakesItDirty_AndTheSavedTextAgainCleansIt)
{
	open_frag();
	const std::size_t before = changed.size();

	BOOST_REQUIRE(document->set_text(std::string(k_frag) + "// more\n"));
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->display_title() == "scene.frag [*]");
	BOOST_TEST(changed.size() > before);
	BOOST_TEST(changed.back().first == 11u);

	BOOST_REQUIRE(document->set_text(k_frag));
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(!document->set_text(k_frag));
}

BOOST_AUTO_TEST_CASE(Save_WritesTheText_AndTheDocumentIsClean)
{
	open_frag();
	BOOST_REQUIRE(document->set_text("#version 450 core\nvoid main() {}\n"));

	const edt::save_result saved = document->save();

	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(text_of(res_root / "shaders/scene.frag") == "#version 450 core\nvoid main() {}\n");
	BOOST_TEST(document->save().saved);
}

BOOST_AUTO_TEST_CASE(Save_KeepsTheFilesCrlfAndBom)
{
	write(res_root, "shaders/crlf.vert", "\xEF\xBB\xBF#version 450\r\nvoid main() {}\r\n");
	document->open(res::tag{ "res://shaders/crlf.vert" });
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_TEST(document->text() == "#version 450\nvoid main() {}\n");

	BOOST_REQUIRE(document->set_text("#version 450\nvoid main() { }\n"));
	BOOST_REQUIRE(document->save().saved);

	BOOST_TEST(text_of(res_root / "shaders/crlf.vert") == "\xEF\xBB\xBF#version 450\r\nvoid main() { }\r\n");
}

BOOST_AUTO_TEST_CASE(Save_IsRefusedWithTheReason_AndNothingIsWritten)
{
	open_frag();
	BOOST_REQUIRE(document->set_text("changed\n"));

	blocked = "a migration runs";
	const edt::save_result by_blocker = document->save();
	BOOST_TEST(!by_blocker.saved);
	BOOST_TEST(by_blocker.error == "a migration runs");
	blocked.reset();

	bool locked = true;
	document->set_lock_source([&locked] { return locked; });
	const edt::save_result in_play = document->save();
	BOOST_TEST(!in_play.saved);
	BOOST_TEST(in_play.error.find("Play") != std::string::npos);
	BOOST_TEST(!document->set_text("more\n"));
	locked = false;

	BOOST_TEST(text_of(res_root / "shaders/scene.frag") == k_frag);
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(AFileOfALowerLayer_OpensReadOnly_AndIsNeverSaved)
{
	write(lower, "shaders/engine.frag", "lower\n");
	document->open(res::tag{ "res://shaders/engine.frag" });

	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_TEST(document->text() == "lower\n");
	BOOST_TEST(document->read_only());
	BOOST_TEST(!document->can_save());
	BOOST_TEST(document->read_only_reason().find("layer below") != std::string::npos);
	BOOST_TEST(!document->set_text("edited\n"));
	BOOST_TEST(!document->save().saved || !document->is_dirty());
	BOOST_TEST(text_of(lower / "shaders/engine.frag") == "lower\n");
}

BOOST_AUTO_TEST_CASE(TheProjectsOwnFile_WinsOverTheSameNameBelow)
{
	write(lower, "shaders/scene.frag", "lower\n");
	open_frag();

	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_TEST(document->text() == k_frag);
	BOOST_TEST(!document->read_only());
}

BOOST_AUTO_TEST_CASE(Save_AFileChangedOnDisk_IsRefused_UntilTheDocumentIsRevertedOrOverwrites)
{
	open_frag();
	write(res_root, "shaders/scene.frag", "somebody else\n");
	BOOST_REQUIRE(document->set_text("mine\n"));

	const edt::save_result refused = document->save();

	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(document->changed_on_disk());
	BOOST_TEST(text_of(res_root / "shaders/scene.frag") == "somebody else\n");

	const edt::save_result forced = document->overwrite();
	BOOST_REQUIRE_MESSAGE(forced.saved, forced.error);
	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(text_of(res_root / "shaders/scene.frag") == "mine\n");
}

BOOST_AUTO_TEST_CASE(Revert_TakesTheFileAsItIsOnDisk_AndDropsTheEdits)
{
	open_frag();
	BOOST_REQUIRE(document->set_text("mine\n"));
	write(res_root, "shaders/scene.frag", "on disk\n");

	BOOST_TEST(document->revert().empty());

	BOOST_TEST(document->text() == "on disk\n");
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(!document->changed_on_disk());
}

BOOST_AUTO_TEST_CASE(AutosaveCapture_IsWhatSaveWrites)
{
	write(res_root, "shaders/crlf.frag", "a\r\nb\r\n");
	document->open(res::tag{ "res://shaders/crlf.frag" });
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_REQUIRE(document->set_text("a\nb\nc\n"));

	const edt::autosave_snapshot snapshot = document->autosave_capture();
	BOOST_REQUIRE(snapshot.error.empty());
	BOOST_REQUIRE(document->save().saved);

	const std::string written = text_of(res_root / "shaders/crlf.frag");
	BOOST_TEST(std::string(reinterpret_cast<const char*>(snapshot.bytes.data()), snapshot.bytes.size()) == written);
}

BOOST_AUTO_TEST_CASE(AClosedDocument_HoldsNothing_AndRefusesToSave)
{
	open_frag();
	document->close();

	BOOST_TEST(!document->is_open());
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(!document->save().saved);
	BOOST_TEST(!document->autosave_capture().error.empty());
}

BOOST_AUTO_TEST_SUITE_END()
