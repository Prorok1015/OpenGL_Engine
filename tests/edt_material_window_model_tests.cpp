#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_material_window_model.h"
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
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

	test_util::temp_dir dir{ "edt_material_window_model" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	edt::editor_services services;
	std::optional<edt::data_document> document;
};

constexpr const char* k_plain_material = R"({"__type":"material_desc","__version":2,"queue":"opaque"})";

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
	BOOST_TEST(derived.shown().at("queue").as_string() == "transparent");
	BOOST_TEST(derived.shown().at("shader").as_string() == "res://shaders/scene.shader.desc");
	const edt::data_document::save_form form = derived.save_bytes();
	BOOST_REQUIRE_MESSAGE(form.error.empty(), form.error);
	BOOST_TEST((form.content == json::parse(derived_text)));
}

BOOST_AUTO_TEST_SUITE_END()
