#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_material_window_model.h"
#include <snk/desc/desc_system.h>
#include <snk/driver/rnd_bind_group.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The material window's model (#1196): the define rows of a material_desc data document, their sources, and the edits of
// `/defines` through the document (one record each, saved like any field).

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif
#ifndef SNK_TEST_PROJECT_DIR
#error "SNK_TEST_PROJECT_DIR must point at projects/sandbox"
#endif

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

struct material_fixture : test_util::scene_env
{
	material_fixture()
		: test_util::scene_env(test_util::test_descs::without)
	{
		fs::create_directories(res_root / "shaders");
		fs::create_directories(lower);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		// The shader_desc a material refers to.
		engine::render::register_render_descs(*descs);
		services.base_descs = descs.get();
		services.base_resources = rs.get();
	}

	~material_fixture()
	{
		engine::render::unregister_render_descs(*descs);
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	res::tag write(const fs::path& root, std::string_view name, std::string_view content) const
	{
		fs::create_directories((root / std::string(name)).parent_path());
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
		return res::tag{ "res://" + std::string(name) };
	}

	res::tag put(std::string_view name, std::string_view content) const { return write(res_root, name, content); }

	static std::string read(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	json::object parsed(std::string_view name) const { return json::parse(read(res_root / std::string(name))).as_object(); }

	// The data document of `tag`, open.
	edt::data_document& open(const res::tag& tag)
	{
		document.emplace(services, layout());
		const edt::open_result opened = document->open(tag);
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		return *document;
	}

	static const edt::define_row* row_of(const std::vector<edt::define_row>& rows, std::string_view name)
	{
		const auto found = std::ranges::find_if(rows, [name](const edt::define_row& row) { return row.name == name; });
		return found == rows.end() ? nullptr : &*found;
	}

	static std::vector<std::string> names_of(const std::vector<edt::define_row>& rows)
	{
		std::vector<std::string> out;
		for (const edt::define_row& row : rows) out.push_back(row.name);
		return out;
	}

	static json::value own_defines(const edt::data_document& data)
	{
		const json::value* field = data.own().if_contains("defines");
		return field ? *field : json::value{};
	}

	static std::string text_of(const std::vector<std::byte>& bytes)
	{
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	test_util::temp_dir dir{ "edt_material_window_model" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	edt::editor_services services;
	std::optional<edt::data_document> document;
};

constexpr const char* k_plain_material = R"({"__type":"material_desc","__version":2,"queue":"opaque"})";
constexpr const char* k_lit_material = R"({"__type":"material_desc","__version":2,"shader":"res://shaders/m.shader.desc"})";

}

BOOST_FIXTURE_TEST_SUITE(EdtMaterialWindowModelTests, material_fixture)

BOOST_AUTO_TEST_CASE(Toggle_OnThenOff_WritesTheOwnDefines_AndTakesThemOutAgain)
{
	edt::data_document& data = open(put("plain.desc", k_plain_material));
	edt::material_window_model model(data, services);
	BOOST_TEST(model.rows().empty());

	BOOST_REQUIRE(model.toggle("USE_FOG"));
	BOOST_TEST((own_defines(data) == json::value(json::array{ "USE_FOG" })));
	const std::vector<edt::define_row> on = model.rows();
	BOOST_REQUIRE(on.size() == 1u);
	BOOST_TEST(on[0].name == "USE_FOG");
	BOOST_TEST((on[0].state == edt::define_state::own_on));
	BOOST_TEST(!on[0].inherited);
	BOOST_TEST(!on[0].from_shader);
	BOOST_TEST(data.is_dirty());

	// Off again is the schema's default (an empty list): the field leaves the file.
	BOOST_REQUIRE(model.toggle("USE_FOG"));
	BOOST_TEST(!data.own().contains("defines"));
	BOOST_TEST(!data.is_dirty());
}

BOOST_AUTO_TEST_CASE(EachToggle_IsOneRecord_UndoAndRedoPutTheDefinesBack)
{
	edt::data_document& data = open(put("plain.desc", R"({"__type":"material_desc","__version":2,"defines":["A_ONE"]})"));
	edt::material_window_model model(data, services);

	BOOST_REQUIRE(model.toggle("B_TWO"));
	BOOST_REQUIRE(model.toggle("A_ONE"));
	BOOST_TEST((own_defines(data) == json::value(json::array{ "B_TWO" })));

	BOOST_REQUIRE(data.undo());
	BOOST_TEST((own_defines(data) == json::value(json::array{ "A_ONE", "B_TWO" })));
	BOOST_REQUIRE(data.undo());
	BOOST_TEST((own_defines(data) == json::value(json::array{ "A_ONE" })));
	BOOST_TEST(!data.is_dirty());
	BOOST_TEST(!data.undo());

	BOOST_REQUIRE(data.redo());
	BOOST_TEST((own_defines(data) == json::value(json::array{ "A_ONE", "B_TWO" })));
}

BOOST_AUTO_TEST_CASE(AnInheritedDefine_IsShownAsInherited_AndTurningItOffWritesAnOverride)
{
	write(lower, "parent.desc", R"({"__type":"material_desc","__version":2,"defines":["USE_SPECULAR_MAP"]})");
	edt::data_document& data = open(put("child.desc", R"({"__type":"material_desc","__version":2,"__parent":"res://parent.desc"})"));
	edt::material_window_model model(data, services);

	const std::vector<edt::define_row> before = model.rows();
	const edt::define_row* inherited = row_of(before, "USE_SPECULAR_MAP");
	BOOST_REQUIRE(inherited);
	BOOST_TEST((inherited->state == edt::define_state::inherited_on));
	BOOST_TEST(inherited->inherited);
	BOOST_TEST(!data.own().contains("defines"));

	BOOST_REQUIRE(model.toggle("USE_SPECULAR_MAP"));
	BOOST_TEST((own_defines(data) == json::value(json::array{})));
	const std::vector<edt::define_row> after = model.rows();
	const edt::define_row* off = row_of(after, "USE_SPECULAR_MAP");
	BOOST_REQUIRE(off);
	BOOST_TEST((off->state == edt::define_state::off));
	BOOST_TEST(off->inherited);

	// On again is what the parent says: the override goes and the define is inherited again.
	BOOST_REQUIRE(model.toggle("USE_SPECULAR_MAP"));
	BOOST_TEST(!data.own().contains("defines"));
	BOOST_TEST((row_of(model.rows(), "USE_SPECULAR_MAP")->state == edt::define_state::inherited_on));
}

BOOST_AUTO_TEST_CASE(TheDefinesTheShaderStagesTest_AreRows_WithoutTheCompilersOwn)
{
	put("shaders/m.vert", "#version 450\n#ifdef USE_ANIMATION\n#endif\n#if defined(USE_FOG) && !defined LOW_QUALITY\n#endif\n");
	put("shaders/m.frag", "#ifdef USE_NORMAL_MAP\n#endif\n#ifndef OPAQUE\n#endif\n  #  ifdef LIGHTS_ENABLED // lit\n#endif\n// #ifdef COMMENTED_OUT\n");
	put("shaders/m.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/m.vert","fragment":"res://shaders/m.frag"})");
	edt::data_document& data = open(put("lit.desc",
		R"({"__type":"material_desc","__version":2,"shader":"res://shaders/m.shader.desc","defines":["USE_NORMAL_MAP","OPAQUE"]})"));
	edt::material_window_model model(data, services);

	const edt::material_shader& shader = model.shader();
	BOOST_TEST_MESSAGE(shader.error);
	BOOST_TEST(shader.error.empty());
	BOOST_TEST(shader.shader.string() == "res://shaders/m.shader.desc");
	BOOST_TEST(shader.vertex.string() == "res://shaders/m.vert");
	BOOST_TEST(shader.fragment.string() == "res://shaders/m.frag");
	BOOST_TEST(!shader.geometry.is_valid());

	const std::vector<edt::define_row> rows = model.rows();
	BOOST_TEST((names_of(rows) == std::vector<std::string>{ "LIGHTS_ENABLED", "LOW_QUALITY", "USE_FOG", "USE_NORMAL_MAP" }));
	for (const edt::define_row& row : rows) {
		BOOST_TEST_CONTEXT(row.name) {
			BOOST_TEST(row.from_shader);
			BOOST_TEST((row.state == (row.name == "USE_NORMAL_MAP" ? edt::define_state::own_on : edt::define_state::off)));
		}
	}

	BOOST_REQUIRE(model.toggle("USE_FOG"));
	BOOST_TEST((row_of(model.rows(), "USE_FOG")->state == edt::define_state::own_on));
}

BOOST_AUTO_TEST_CASE(ScanDefineNames_ReadsIfdefIfndefAndDefined_OnceEach)
{
	const std::vector<std::string> names = edt::scan_define_names(
		"#ifdef A\n#ifndef B\n#if defined(C) || defined D\n#elif defined ( E )\n#if FOO_defined(X)\n#if undefinedY\n#ifdef A\n");
	BOOST_TEST((names == std::vector<std::string>{ "A", "B", "C", "D", "E" }));
	BOOST_TEST(edt::scan_define_names("#define A 1\nint x;\n").empty());
}

BOOST_AUTO_TEST_CASE(Add_RefusesWhatIsNoIdentifier_AndTheCompilersDefines)
{
	edt::data_document& data = open(put("plain.desc", k_plain_material));
	edt::material_window_model model(data, services);

	for (const char* bad : { "", "1ST", "BAD-NAME", "TWO WORDS", "A=1" }) {
		BOOST_TEST_CONTEXT(bad) {
			BOOST_TEST(!edt::is_define_name(bad));
			BOOST_TEST(!model.add(bad).empty());
		}
	}
	BOOST_TEST(!model.add("OPAQUE").empty());
	BOOST_TEST(!model.add("USE_ANIMATION").empty());
	// #1439: the compiler adds the alpha test from alpha_cutoff, so it is not a define of the material.
	BOOST_TEST(edt::is_compiler_define("ALPHA_TEST"));
	BOOST_TEST(!model.add("ALPHA_TEST").empty());
	BOOST_TEST(!data.own().contains("defines"));
	BOOST_TEST(!data.is_dirty());

	BOOST_TEST(model.add("_MY_DEFINE2").empty());
	BOOST_TEST((own_defines(data) == json::value(json::array{ "_MY_DEFINE2" })));
	BOOST_TEST(!model.add("_MY_DEFINE2").empty());

	// An added name stays a row once it is off, until it is removed.
	BOOST_REQUIRE(model.toggle("_MY_DEFINE2"));
	const std::vector<edt::define_row> rows = model.rows();
	const edt::define_row* added = row_of(rows, "_MY_DEFINE2");
	BOOST_REQUIRE(added);
	BOOST_TEST((added->state == edt::define_state::off));
	BOOST_TEST(!model.remove("_MY_DEFINE2"));
	BOOST_TEST(!row_of(model.rows(), "_MY_DEFINE2"));
}

BOOST_AUTO_TEST_CASE(Save_WritesTheDefines_AndAReloadReadsThemBack)
{
	write(lower, "parent.desc", R"({"__type":"material_desc","__version":2,"defines":["USE_SPECULAR_MAP"]})");
	const res::tag tag = put("child.desc", R"({"__type":"material_desc","__version":2,"__parent":"res://parent.desc"})");
	{
		edt::data_document& data = open(tag);
		edt::material_window_model model(data, services);
		BOOST_REQUIRE(model.add("USE_FOG").empty());
		const edt::save_result saved = data.save();
		BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	}
	const json::object file = parsed("child.desc");
	BOOST_TEST((file.at("defines") == json::value(json::array{ "USE_SPECULAR_MAP", "USE_FOG" })));

	edt::data_document& reloaded = open(tag);
	edt::material_window_model model(reloaded, services);
	const std::vector<edt::define_row> rows = model.rows();
	BOOST_TEST((names_of(rows) == std::vector<std::string>{ "USE_FOG", "USE_SPECULAR_MAP" }));
	for (const edt::define_row& row : rows) {
		BOOST_TEST_CONTEXT(row.name) { BOOST_TEST((row.state == edt::define_state::own_on)); }
	}
}

// The shipped materials open as documents and would be written back as they are (#1196): a base material, and a derived one
// whose parent is in a lower layer.
BOOST_AUTO_TEST_CASE(TheShippedBaseAndDerivedMaterials_OpenAsDocuments_AndSaveUnchanged)
{
	const std::string base_text = read(fs::path{ SNK_CORE_RES_DIR } / "base_material.desc");
	const std::string derived_text = read(fs::path{ SNK_TEST_PROJECT_DIR } / "res" / "materials" / "derived_material.desc");
	BOOST_REQUIRE(!base_text.empty());
	BOOST_REQUIRE(!derived_text.empty());

	{
		edt::data_document& base = open(put("base_material.desc", base_text));
		BOOST_TEST(!base.migrated());
		const edt::data_document::save_form form = base.save_bytes();
		BOOST_REQUIRE_MESSAGE(form.error.empty(), form.error);
		BOOST_TEST((form.content == json::parse(base_text)));
		fs::remove(res_root / "base_material.desc");
	}

	write(lower, "base_material.desc", base_text);
	edt::data_document& derived = open(put("materials/derived_material.desc", derived_text));
	BOOST_TEST(!derived.migrated());
	// #1440: drawn in both queues with the shader that has a transparent path.
	BOOST_TEST(derived.shown().at("queue").as_string() == "mix");
	BOOST_TEST(derived.shown().at("shader").as_string() == "res://shaders/mix_opaque_trans_scene.shader.desc");
	const edt::data_document::save_form form = derived.save_bytes();
	BOOST_REQUIRE_MESSAGE(form.error.empty(), form.error);
	BOOST_TEST((form.content == json::parse(derived_text)));
}

BOOST_AUTO_TEST_CASE(InheritedDefines_OffThenOn_InAnyOrder_LeaveNoOwnField)
{
	write(lower, "parent.desc", R"({"__type":"material_desc","__version":2,"defines":["A_ONE","B_TWO"]})");
	edt::data_document& data = open(put("child.desc", R"({"__type":"material_desc","__version":2,"__parent":"res://parent.desc"})"));
	edt::material_window_model model(data, services);

	// B_TWO off and on: the list it would write is the parent's.
	BOOST_REQUIRE(model.toggle("B_TWO"));
	BOOST_TEST((own_defines(data) == json::value(json::array{ "A_ONE" })));
	BOOST_REQUIRE(model.toggle("B_TWO"));
	BOOST_TEST(!data.own().contains("defines"));

	// A_ONE off and on: on again appends it, ["B_TWO", "A_ONE"], the parent's entries in another order.
	BOOST_REQUIRE(model.toggle("A_ONE"));
	BOOST_TEST((own_defines(data) == json::value(json::array{ "B_TWO" })));
	BOOST_REQUIRE(model.toggle("A_ONE"));
	BOOST_TEST(!data.own().contains("defines"));
	BOOST_TEST(!data.is_dirty());
	for (const edt::define_row& row : model.rows()) {
		BOOST_TEST_CONTEXT(row.name) { BOOST_TEST((row.state == edt::define_state::inherited_on)); }
	}
}

BOOST_AUTO_TEST_CASE(ResetToInherited_DropsTheOwnDefines_InOneRecord)
{
	write(lower, "parent.desc", R"({"__type":"material_desc","__version":2,"defines":["USE_SPECULAR_MAP"]})");
	edt::data_document& data = open(put("child.desc", R"({"__type":"material_desc","__version":2,"__parent":"res://parent.desc"})"));
	edt::material_window_model model(data, services);
	BOOST_TEST(!model.can_reset_to_inherited());
	BOOST_TEST(!model.reset_to_inherited());

	BOOST_REQUIRE(model.toggle("USE_FOG"));
	BOOST_TEST((own_defines(data) == json::value(json::array{ "USE_SPECULAR_MAP", "USE_FOG" })));
	BOOST_REQUIRE(model.can_reset_to_inherited());

	BOOST_REQUIRE(model.reset_to_inherited());
	BOOST_TEST(!data.own().contains("defines"));
	BOOST_TEST(!model.can_reset_to_inherited());
	const std::vector<edt::define_row> rows = model.rows();
	BOOST_TEST((names_of(rows) == std::vector<std::string>{ "USE_SPECULAR_MAP" }));
	BOOST_TEST((rows.at(0).state == edt::define_state::inherited_on));

	BOOST_REQUIRE(data.undo());
	BOOST_TEST((own_defines(data) == json::value(json::array{ "USE_SPECULAR_MAP", "USE_FOG" })));
}

BOOST_AUTO_TEST_CASE(ADefineWithAValue_OffThenOn_GetsItsValueBack)
{
	edt::data_document& data = open(put("plain.desc", R"({"__type":"material_desc","__version":2,"defines":["MAX_LIGHT_COUNT 4"]})"));
	edt::material_window_model model(data, services);
	BOOST_TEST((row_of(model.rows(), "MAX_LIGHT_COUNT")->state == edt::define_state::own_on));

	BOOST_REQUIRE(model.toggle("MAX_LIGHT_COUNT"));
	BOOST_TEST(!data.own().contains("defines"));
	BOOST_REQUIRE(model.toggle("MAX_LIGHT_COUNT"));
	BOOST_TEST((own_defines(data) == json::value(json::array{ "MAX_LIGHT_COUNT 4" })));
	BOOST_TEST(!data.is_dirty());
}

BOOST_AUTO_TEST_CASE(ScanDefineNames_SkipsBlockComments_AndWhatTheSourceDefines)
{
	BOOST_TEST((edt::scan_define_names("/* #ifdef HIDDEN\n#ifdef ALSO_HIDDEN */\n#ifdef SHOWN\n#endif\n/* #if defined(INLINE_HIDDEN) */\n")
		== std::vector<std::string>{ "SHOWN" }));
	BOOST_TEST((edt::scan_define_names("#ifndef MY_GUARD\n#define MY_GUARD\n#ifdef REAL\n#endif\n#if defined(PLATFORM_ES)\n#endif\n#define PLATFORM_ES 1\n#endif\n")
		== std::vector<std::string>{ "REAL" }));
}

BOOST_AUTO_TEST_CASE(AnIncludedFilesDefines_AreRows_WithoutItsGuard)
{
	put("shaders/lib/common.glsl", "#ifdef FROM_INCLUDE\n#endif\n");
	put("shaders/m.vert", "#version 450\n");
	put("shaders/m.frag", "#version 450\n#include \"lib/common.glsl\"\n#ifdef OWN_STAGE\n#endif\n");
	put("shaders/m.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/m.vert","fragment":"res://shaders/m.frag"})");
	edt::data_document& data = open(put("lit.desc", k_lit_material));
	edt::material_window_model model(data, services);

	BOOST_TEST_MESSAGE(model.shader().error);
	BOOST_TEST(model.shader().error.empty());
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "FROM_INCLUDE", "OWN_STAGE" }));

	// The included file is watched like the stages: a change of it is read again.
	const res::tag library = put("shaders/lib/common.glsl", "#ifdef CHANGED_INCLUDE\n#endif\n");
	rs->signal_changed(library);
	pump();
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "CHANGED_INCLUDE", "OWN_STAGE" }));
}

BOOST_AUTO_TEST_CASE(AMissingStage_IsAnError_TheOtherStagesStillGiveRows_AndItIsReadOnceItAppears)
{
	put("shaders/m.frag", "#ifdef KEPT\n#endif\n");
	put("shaders/m.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/gone.vert","fragment":"res://shaders/m.frag"})");
	edt::data_document& data = open(put("lit.desc", k_lit_material));
	edt::material_window_model model(data, services);

	BOOST_TEST(model.shader().error.find("gone.vert") != std::string::npos);
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "KEPT" }));

	const res::tag vertex = put("shaders/gone.vert", "#ifdef BACK\n#endif\n");
	rs->signal_changed(vertex);
	pump();
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "BACK", "KEPT" }));
	BOOST_TEST(model.shader().error.empty());
}

BOOST_AUTO_TEST_CASE(AStageThatChanges_IsReadAgain_AndATypedShaderOnlyOnceCommitted)
{
	put("shaders/m.vert", "#version 450\n");
	const res::tag fragment = put("shaders/m.frag", "#ifdef FIRST\n#endif\n");
	put("shaders/m.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/m.vert","fragment":"res://shaders/m.frag"})");
	put("shaders/o.frag", "#ifdef OTHER\n#endif\n");
	put("shaders/o.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/m.vert","fragment":"res://shaders/o.frag"})");
	edt::data_document& data = open(put("lit.desc", k_lit_material));
	edt::material_window_model model(data, services);
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "FIRST" }));

	put("shaders/m.frag", "#ifdef SECOND\n#endif\n");
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "FIRST" }));
	rs->signal_changed(fragment);
	pump();
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "SECOND" }));

	// A value typed in and not committed yet (a field still active) is not read.
	BOOST_REQUIRE(data.edit([](json::value& shown) {
		shown.as_object()["shader"] = "res://shaders/o.shader.desc";
		return true;
	}));
	BOOST_REQUIRE(data.has_pending_edit());
	BOOST_TEST(model.shader().shader.string() == "res://shaders/m.shader.desc");
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "SECOND" }));

	data.commit_pending();
	BOOST_TEST(model.shader().shader.string() == "res://shaders/o.shader.desc");
	BOOST_TEST((names_of(model.rows()) == std::vector<std::string>{ "OTHER" }));
}

// A toggle on and off again leaves the file as it was, the fields the window does not know included (#1196).
BOOST_AUTO_TEST_CASE(ADefineOnAndOffAgain_SavesTheFileByteForByte)
{
	put("textures/spec.png", "not really a png");
	const auto canonical = [this](std::string_view text) { return text_of(descs->serialize_to_bytes(json::parse(text))); };
	const std::string known = canonical(R"({"__type":"material_desc","__version":2,"__guid":"5d3c1a0e-8f7b-4c2d-9e61-0a4b3c2d1e0f",)"
		R"("samplers":[null,"res://textures/spec.png"],"queue":"opaque"})");
	const std::string unknown = canonical(R"({"__type":"material_desc","__version":2,"__guid":"5d3c1a0e-8f7b-4c2d-9e61-0a4b3c2d1e0f",)"
		R"("samplers":[null,"res://textures/spec.png"],"queue":"opaque","not_a_field":{"kept":true}})");

	for (const std::string& original : { known, unknown }) {
		BOOST_TEST_CONTEXT(original) {
			const res::tag tag = put("round.desc", original);
			edt::data_document& data = open(tag);
			edt::material_window_model model(data, services);
			BOOST_REQUIRE(model.toggle("USE_FOG"));
			BOOST_TEST(data.is_dirty());
			BOOST_REQUIRE(model.toggle("USE_FOG"));
			BOOST_TEST(!data.is_dirty());
			const edt::save_result saved = data.save();
			BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
			BOOST_TEST(read(res_root / "round.desc") == original);
			// What a forced write would give is the same bytes (a field the schema does not know is refused by design).
			if (original == known) {
				const edt::data_document::save_form form = data.save_bytes();
				BOOST_REQUIRE_MESSAGE(form.error.empty(), form.error);
				BOOST_TEST(text_of(form.bytes) == original);
			}
		}
	}
}

namespace {

constexpr const char* k_defined_shader = R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/m.vert","fragment":"res://shaders/m.frag","defines":["SHADER_LEVEL 1"]})";
constexpr const char* k_fog_material = R"({"__type":"material_desc","__version":2,"shader":"res://shaders/m.shader.desc","defines":["USE_FOG"]})";

rnd::shader_problem problem_at(std::string_view file, int line, std::string_view program, std::string_view code = "shader/compile")
{
	rnd::shader_problem out;
	out.file = res::tag{ std::string{ file } };
	out.line = line;
	out.code = std::string{ code };
	out.message = std::format("error at {}", line);
	out.program = std::string{ program };
	return out;
}

const edt::compile_row* row_at(const std::vector<edt::compile_row>& rows, int line)
{
	const auto found = std::ranges::find_if(rows, [line](const edt::compile_row& row) { return row.problem.line == line; });
	return found == rows.end() ? nullptr : &*found;
}

bool any_holds(const std::vector<std::string>& messages, std::string_view text)
{
	return std::ranges::any_of(messages, [text](const std::string& message) { return message.find(text) != std::string::npos; });
}

}

BOOST_AUTO_TEST_CASE(CompileRows_ThisMaterialsSetAndTags_AreItsOwn_OtherSetsAreOthers_UnrelatedFilesAreLeftOut)
{
	put("shaders/lib/common.glsl", "float shade() { return 1.0; }\n");
	put("shaders/m.vert", "#version 450\n");
	put("shaders/m.frag", "#version 450\n#include \"lib/common.glsl\"\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc", k_fog_material));
	edt::material_window_model model(data, services);
	model.scan();
	const res::tag preview{ "memory://editor/material/77.desc" };

	const std::vector<rnd::shader_problem> problems{
		// The material's set in another order (constants come from a hash map), then its opaque variant: one row.
		problem_at("res://shaders/m.frag", 3, "m.vert + m.frag { USE_FOG | SHADER_LEVEL 1 }"),
		problem_at("res://shaders/m.frag", 3, "m.vert + m.frag { SHADER_LEVEL 1 | USE_FOG | OPAQUE }"),
		problem_at("res://shaders/m.frag", 4, "m.vert + m.frag { SHADER_LEVEL 1 | USE_FOG | USE_ANIMATION | OPAQUE }"),
		problem_at("res://shaders/m.frag", 9, "m.vert + m.frag { SHADER_LEVEL 1 }"),
		problem_at("res://shaders/lib/common.glsl", 1, "m.vert + m.frag { SHADER_LEVEL 1 | OTHER }"),
		problem_at("res://shaders/x.frag", 5, "x.vert + x.frag { SHADER_LEVEL 1 | USE_FOG }"),
		problem_at("res://shaders/m.shader.desc", 0, "res://lit.desc", "shader/load"),
		problem_at("res://shaders/m.shader.desc", 0, "memory://editor/material/77.desc", "shader/load"),
		problem_at("res://shaders/m.shader.desc", 0, "res://other.desc", "shader/load"),
	};
	const edt::compile_errors errors = model.compile_rows(problems, preview);

	BOOST_TEST(!errors.sky);
	BOOST_REQUIRE(errors.material.size() == 3u);
	const edt::compile_row* base = row_at(errors.material, 3);
	BOOST_REQUIRE(base);
	BOOST_TEST(!base->skinned);
	BOOST_TEST(!base->preprocess);
	const edt::compile_row* skinned = row_at(errors.material, 4);
	BOOST_REQUIRE(skinned);
	BOOST_TEST(skinned->skinned);
	// The document's load row and the preview's, merged: the same place and message.
	const edt::compile_row* load = row_at(errors.material, 0);
	BOOST_REQUIRE(load);
	BOOST_TEST(load->problem.code == "shader/load");

	BOOST_REQUIRE(errors.others.size() == 2u);
	const edt::compile_row* other = row_at(errors.others, 9);
	BOOST_REQUIRE(other);
	BOOST_TEST(other->define_set == "{ SHADER_LEVEL 1 }");
	const edt::compile_row* included = row_at(errors.others, 1);
	BOOST_REQUIRE(included);
	BOOST_TEST(included->problem.file.string() == "res://shaders/lib/common.glsl");
	BOOST_TEST(included->define_set == "{ SHADER_LEVEL 1 | OTHER }");
	for (const edt::compile_row& row : errors.others) {
		BOOST_TEST(row.problem.file.string() != "res://shaders/x.frag");
	}
}

// #1439: the alpha test define is the compiler's, like the pass and skinning ones: a program with it is still this material's.
BOOST_AUTO_TEST_CASE(CompileRows_TheAlphaTestDefine_IsTheCompilers)
{
	put("shaders/m.vert", "#version 450\n");
	put("shaders/m.frag", "#version 450\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc", k_fog_material));
	edt::material_window_model model(data, services);
	model.scan();

	const std::vector<rnd::shader_problem> problems{
		problem_at("res://shaders/m.frag", 3, "m.vert + m.frag { USE_FOG | SHADER_LEVEL 1 | ALPHA_TEST }"),
		problem_at("res://shaders/m.frag", 5, "m.vert + m.frag { ALPHA_TEST | SHADER_LEVEL 1 | USE_FOG | OPAQUE }"),
	};
	const edt::compile_errors errors = model.compile_rows(problems, res::tag{});
	BOOST_TEST(errors.material.size() == 2u);
	BOOST_TEST(errors.others.empty());
}

BOOST_AUTO_TEST_CASE(CompileRows_ASharedIncludesRowOfAnotherShadersProgram_IsOthers_EvenWithTheSameDefineSet)
{
	put("shaders/lib/lights.glsl", "float light() { return 1.0; }\n");
	put("shaders/a.vert", "#version 450\n");
	put("shaders/a.frag", "#version 450\n#include \"lib/lights.glsl\"\n");
	put("shaders/b.vert", "#version 450\n");
	put("shaders/b.frag", "#version 450\n#include \"lib/lights.glsl\"\n");
	put("shaders/a.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag"})");
	put("shaders/b.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/b.vert","fragment":"res://shaders/b.frag"})");
	edt::data_document& data = open(put("a.desc", R"({"__type":"material_desc","__version":2,"shader":"res://shaders/a.shader.desc"})"));
	edt::material_window_model model(data, services);
	model.scan();

	const edt::compile_errors errors = model.compile_rows({
		problem_at("res://shaders/lib/lights.glsl", 1, "a.vert + a.frag"),
		problem_at("res://shaders/lib/lights.glsl", 2, "b.vert + b.frag"),
	}, res::tag{});
	BOOST_REQUIRE(errors.material.size() == 1u);
	BOOST_TEST(errors.material[0].problem.line == 1);
	BOOST_REQUIRE(errors.others.size() == 1u);
	BOOST_TEST(errors.others[0].problem.line == 2);
	BOOST_TEST(errors.others[0].define_set == "no defines");
}

BOOST_AUTO_TEST_CASE(CompileRows_ThePreviewsLoadRowAlone_IsThisMaterials)
{
	put("shaders/m.vert", "#version 450\n");
	put("shaders/m.frag", "#version 450\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc", k_fog_material));
	edt::material_window_model model(data, services);
	model.scan();
	const res::tag preview{ "memory://editor/material/78.desc" };

	const edt::compile_errors errors = model.compile_rows(
		{ problem_at("res://shaders/m.shader.desc", 0, "memory://editor/material/78.desc", "shader/load") }, preview);
	BOOST_TEST(errors.material.size() == 1u);
	BOOST_TEST(errors.others.empty());
	BOOST_TEST(model.compile_rows({ problem_at("res://shaders/m.shader.desc", 0, "memory://editor/material/78.desc", "shader/load") }, res::tag{}).material.empty());
}

BOOST_AUTO_TEST_CASE(Errors_AreMadeAgain_OnlyWhenTheGenerationOrTheDefinesChange)
{
	put("shaders/m.vert", "#version 450\n");
	put("shaders/m.frag", "#version 450\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc", k_fog_material));
	edt::material_window_model model(data, services);
	model.scan();
	int asked = 0;
	const auto snapshot = [&asked] {
		++asked;
		return std::vector<rnd::shader_problem>{ problem_at("res://shaders/m.frag", 2, "m.vert + m.frag { SHADER_LEVEL 1 | USE_FOG }") };
	};
	BOOST_TEST(model.errors(1, snapshot, res::tag{}).material.size() == 1u);
	BOOST_TEST(model.errors(1, snapshot, res::tag{}).material.size() == 1u);
	BOOST_TEST(asked == 1);
	// USE_FOG off: the same row is another variant now.
	BOOST_REQUIRE(model.toggle("USE_FOG"));
	model.scan();
	const edt::compile_errors& after = model.errors(1, snapshot, res::tag{});
	BOOST_TEST(asked == 2);
	BOOST_TEST(after.material.empty());
	BOOST_TEST(after.others.size() == 1u);
	model.errors(2, snapshot, res::tag{});
	BOOST_TEST(asked == 3);
}

BOOST_AUTO_TEST_CASE(PreCheck_WarnsOfBadEntries_AndOfNamesTheEngineOrTheShaderDefine)
{
	put("shaders/m.vert", "#version 450\n");
	// Defaults under their own guard are the material's to set; an unguarded #define is not.
	put("shaders/m.frag", "#version 450\n#define SOURCE_MACRO 1\n#ifndef GUARDED\n#define GUARDED 1\n#endif\n"
		"#if !defined(PAREN_GUARDED)\n#define PAREN_GUARDED 2\n#endif\n#if !defined BARE_GUARDED\n#define BARE_GUARDED 3\n#endif\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc",
		R"({"__type":"material_desc","__version":2,"shader":"res://shaders/m.shader.desc","defines":)"
		R"(["BAD-NAME","LEVEL 1 2","MAX_LIGHT_COUNT 4","SNK_FRAGMENT_SHADER","SHADER_LEVEL 2","SOURCE_MACRO","FINE 3",)"
		R"("EQUALS=1","SPLIT 1\n#error","GUARDED 4","PAREN_GUARDED","BARE_GUARDED 5"]})"));
	edt::material_window_model model(data, services);
	model.scan();

	const std::vector<std::string> warnings = model.pre_check();
	BOOST_TEST(any_holds(warnings, "'BAD-NAME' does not start with a define name"));
	BOOST_TEST(any_holds(warnings, "'LEVEL 1 2': unsupported by the program title format: one value token only"));
	BOOST_TEST(any_holds(warnings, "'SPLIT 1\n#error': unsupported by the program title format: one value token only"));
	BOOST_TEST(any_holds(warnings, "'EQUALS=1': use 'NAME VALUE', not 'NAME=VALUE'"));
	BOOST_TEST(!any_holds(warnings, "GUARDED"));
	BOOST_TEST(any_holds(warnings, "MAX_LIGHT_COUNT is defined by the engine"));
	BOOST_TEST(any_holds(warnings, "SNK_FRAGMENT_SHADER is defined by the engine"));
	BOOST_TEST(any_holds(warnings, "'SHADER_LEVEL 2' replaces the shader's define 'SHADER_LEVEL 1'"));
	BOOST_TEST(any_holds(warnings, "SOURCE_MACRO is #defined by the shader's source"));
	BOOST_TEST(!any_holds(warnings, "FINE"));
	BOOST_TEST(warnings.size() == 8u);
}

BOOST_AUTO_TEST_CASE(Preprocess_WaitsForTheDelay_RunsOnce_AndAnEditRestartsTheWait)
{
	put("shaders/m.vert", "#version 450\n");
	// No #version: each run finds one problem, so the rows of a run can be told from none.
	put("shaders/m.frag", "void main() {}\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc", k_fog_material));
	edt::material_window_model model(data, services);
	model.scan();
	const auto& bindings = rnd::driver::per_type_binding_table;
	using std::chrono::milliseconds;
	const std::chrono::steady_clock::time_point t0{};

	BOOST_TEST(!model.preprocess(t0, bindings));
	BOOST_TEST(!model.preprocess(t0 + milliseconds(299), bindings));
	BOOST_TEST(model.preprocess(t0 + milliseconds(300), bindings));
	BOOST_TEST(model.compile_rows({}, res::tag{}).material.size() == 1u);
	BOOST_TEST(!model.preprocess(t0 + milliseconds(1000), bindings));

	// An edit, then another before the delay is over: the wait starts again from the second.
	const auto t1 = t0 + milliseconds(2000);
	BOOST_REQUIRE(model.toggle("USE_FOG"));
	model.scan();
	BOOST_TEST(!model.preprocess(t1, bindings));
	// The old define set's rows are gone at once, not when the new run comes.
	BOOST_TEST(model.compile_rows({}, res::tag{}).material.empty());
	BOOST_REQUIRE(model.toggle("USE_FOG"));
	model.scan();
	BOOST_TEST(!model.preprocess(t1 + milliseconds(200), bindings));
	BOOST_TEST(!model.preprocess(t1 + milliseconds(300), bindings));
	BOOST_TEST(!model.preprocess(t1 + milliseconds(499), bindings));
	BOOST_TEST(model.compile_rows({}, res::tag{}).material.empty());
	BOOST_TEST(model.preprocess(t1 + milliseconds(500), bindings));
	BOOST_TEST(model.compile_rows({}, res::tag{}).material.size() == 1u);
}

BOOST_AUTO_TEST_CASE(PreprocessProblems_AreThisMaterials_MarkedPreprocess)
{
	put("shaders/m.vert", "#version 450\n");
	put("shaders/m.frag", "void main() {}\n");
	put("shaders/m.shader.desc", k_defined_shader);
	edt::data_document& data = open(put("lit.desc", k_fog_material));
	edt::material_window_model model(data, services);
	model.scan();
	BOOST_TEST(model.compile_rows({}, res::tag{}).material.empty());

	const std::chrono::steady_clock::time_point t0{};
	model.preprocess(t0, rnd::driver::per_type_binding_table);
	BOOST_REQUIRE(model.preprocess(t0 + edt::material_window_model::preprocess_delay, rnd::driver::per_type_binding_table));
	const edt::compile_errors errors = model.compile_rows({}, res::tag{});
	BOOST_REQUIRE(errors.material.size() == 1u);
	BOOST_TEST(errors.material[0].preprocess);
	BOOST_TEST(errors.material[0].problem.file.string() == "res://shaders/m.frag");
	BOOST_TEST(errors.material[0].problem.code == "shader/load");
	BOOST_TEST(errors.material[0].define_set == "{ SHADER_LEVEL 1 | USE_FOG }");
	BOOST_TEST(errors.others.empty());
}

BOOST_AUTO_TEST_CASE(ASkyMaterial_GetsTheLastFrameNote)
{
	put("shaders/sky.vert", "#version 450\n");
	put("shaders/sky.frag", "#version 450\n");
	put("shaders/sky.shader.desc", R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/sky.vert","fragment":"res://shaders/sky.frag"})");
	edt::data_document& data = open(put("sky.desc", R"({"__type":"material_desc","__version":2,"shader":"res://shaders/sky.shader.desc"})"));
	edt::material_window_model model(data, services);
	model.scan();
	const edt::compile_errors errors = model.compile_rows({ problem_at("res://shaders/sky.frag", 2, "sky.vert + sky.frag") }, res::tag{});
	BOOST_TEST(errors.sky);
	BOOST_TEST(errors.material.size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
