#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "test_fake_asset_catalog.hpp"
#include "edt_asset_picker.h"
#include "edt_data_document.h"
#include "edt_document_creator.h"
#include "edt_texture_slot_model.h"
#include "edt_texture_viewer_host.h"
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The texture slots of the material window (#1205): Diffuse / Specular / Normal are /samplers/0..2 of a material_desc data
// document, each change one record of its history, the list saved as material_desc reads it; a picture dropped on a slot
// stands for the texture desc Create Texture made for it.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

struct slot_fixture : test_util::scene_env
{
	slot_fixture()
		: test_util::scene_env(test_util::test_descs::without)
	{
		fs::create_directories(res_root);
		fs::create_directories(lower);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		engine::render::register_render_descs(*descs);
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		creator = std::make_unique<edt::document_creator>(edt::document_creator::ports{
			[this] { return descs.get(); }, [this] { return rs.get(); },
			[this] { return std::vector<fs::path>{ lower, res_root }; },
			[]() -> std::optional<std::string> { return std::nullopt; } });
	}

	~slot_fixture()
	{
		document.reset();
		creator.reset();
		engine::render::unregister_render_descs(*descs);
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout() const
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	static res::tag write(const fs::path& root, std::string_view name, std::string_view content)
	{
		fs::create_directories((root / std::string(name)).parent_path());
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
		return res::tag{ "res://" + std::string(name) };
	}

	res::tag put(std::string_view name, std::string_view content) const { return write(res_root, name, content); }

	edt::data_document& open(const res::tag& tag)
	{
		document.emplace(services, layout());
		const edt::open_result opened = document->open(tag);
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		return *document;
	}

	static json::value own_samplers(const edt::data_document& data)
	{
		const json::value* field = data.own().if_contains("samplers");
		return field ? *field : json::value{};
	}

	static const edt::texture_slot& slot(std::size_t index) { return edt::material_texture_slots()[index]; }

	res::tag texture_of(const res::tag& picture) const
	{
		return edt::existing_picture_texture(*creator, *rs, *descs, layout(), picture);
	}

	test_util::temp_dir dir{ "edt_texture_slot_model" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	edt::editor_services services;
	std::unique_ptr<edt::document_creator> creator;
	std::optional<edt::data_document> document;
};

constexpr const char* k_plain_material = R"({"__type":"material_desc","__version":2,"queue":"opaque"})";

json::value list(std::initializer_list<json::value> items)
{
	json::array out;
	for (const json::value& item : items) out.push_back(item);
	return out;
}

json::value slotted(const json::value& samplers, std::size_t index, json::value value)
{
	return edt::with_slot(samplers, index, std::move(value));
}

}

BOOST_FIXTURE_TEST_SUITE(EdtTextureSlotModelTests, slot_fixture)

BOOST_AUTO_TEST_CASE(TheSlots_AreDiffuseSpecularNormal_AtTheirPlacesInSamplers)
{
	BOOST_REQUIRE(edt::material_texture_slots().size() == 3u);
	BOOST_TEST(slot(0).name == "Diffuse");
	BOOST_TEST(slot(1).name == "Specular");
	BOOST_TEST(slot(2).name == "Normal");
	for (std::size_t i = 0; i < 3; ++i) BOOST_TEST(slot(i).index == i);
}

BOOST_AUTO_TEST_CASE(WithSlot_FillsAGapWithNull_AndTrimsTheEmptySlotsAtTheEnd)
{
	BOOST_TEST((slotted(json::value{}, 2, "n") == list({ nullptr, nullptr, "n" })));
	BOOST_TEST((slotted(list({ nullptr, nullptr, "n" }), 0, "d") == list({ "d", nullptr, "n" })));
	BOOST_TEST((slotted(list({ "d", nullptr, "n" }), 2, nullptr) == list({ "d" })));
	BOOST_TEST((slotted(list({ "d", nullptr, "n" }), 0, nullptr) == list({ nullptr, nullptr, "n" })));
	BOOST_TEST(edt::with_slot(list({ "d" }), 0, nullptr).empty());
	BOOST_TEST((slotted(list({ "d", nullptr, "n", "x" }), 1, "s") == list({ "d", "s", "n", "x" })));
}

// A picture stands for its texture desc only when Create Texture's desc beside it is there and names it.
BOOST_AUTO_TEST_CASE(APicture_IsTakenAsTheTextureDescThatNamesIt_ElseCreateTextureFirst)
{
	put("pictures/rock.png", "not decoded here");
	put("pictures/rock.txm.desc", R"({"__type":"texture_2d_desc","__parent":"res://base_texture.desc","data":"res://pictures/rock.png"})");
	put("pictures/moss.png", "not decoded here");
	put("pictures/moss.txm.desc", R"({"__type":"texture_2d_desc","__parent":"res://base_texture.desc","data":"res://pictures/rock.png"})");
	put("pictures/bare.png", "not decoded here");

	BOOST_TEST(texture_of(res::tag{ "res://pictures/rock.png" }).view() == "res://pictures/rock.txm.desc");
	BOOST_TEST(!texture_of(res::tag{ "res://pictures/moss.png" }).is_valid());
	BOOST_TEST(!texture_of(res::tag{ "res://pictures/bare.png" }).is_valid());

	test_util::fake_catalog source;
	source.add("pictures/rock.png", 2, "texture");
	source.add("pictures/rock.txm.desc", 2, "desc", "texture_2d_desc");
	source.add("pictures/moss.png", 2, "texture");
	source.add("pictures/bare.png", 2, "texture");
	const auto lookup = [this](const res::tag& picture) { return texture_of(picture); };

	const edt::picker::texture_choice rock = edt::picker::choose_texture("pictures/rock.png", &source, lookup);
	BOOST_TEST(rock.answer.fits());
	BOOST_TEST((rock.value == json::value("res://pictures/rock.txm.desc")));
	for (const char* refused : { "pictures/moss.png", "pictures/bare.png" }) {
		BOOST_TEST_CONTEXT(refused) {
			const edt::picker::texture_choice choice = edt::picker::choose_texture(refused, &source, lookup);
			BOOST_TEST((choice.answer.outcome == edt::picker::verdict::result::rejected));
			BOOST_TEST(choice.answer.reason == "Create Texture first");
		}
	}
	// The picker wrote nothing.
	BOOST_TEST(!fs::exists(res_root / "pictures" / "bare.txm.desc"));
}

BOOST_AUTO_TEST_CASE(ADescOfAnotherTypeBesideThePicture_IsNotItsTexture)
{
	put("pictures/leaf.png", "not decoded here");
	put("pictures/leaf.txm.desc", R"({"__type":"material_desc","__version":2,"data":"res://pictures/leaf.png"})");
	BOOST_TEST(!texture_of(res::tag{ "res://pictures/leaf.png" }).is_valid());
}

// A failed thumbnail load is not tried again (each try logs an error) until the slot names another texture or the asset
// list moves.
BOOST_AUTO_TEST_CASE(AFailedThumbnail_IsNotTriedAgain_UntilTheTextOrTheRevisionChanges)
{
	edt::thumbnail_attempts attempts;
	BOOST_TEST(attempts.may_try(0, "res://broken.txm.desc", 1));
	attempts.fail(0, "res://broken.txm.desc", 1);
	BOOST_TEST(!attempts.may_try(0, "res://broken.txm.desc", 1));
	BOOST_TEST(!attempts.may_try(0, "res://broken.txm.desc", 1));
	BOOST_TEST(attempts.may_try(1, "res://broken.txm.desc", 1));
	BOOST_TEST(attempts.may_try(0, "res://other.txm.desc", 1));
	BOOST_TEST(attempts.may_try(0, "res://broken.txm.desc", 2));
	attempts.fail(0, "res://broken.txm.desc", 2);
	BOOST_TEST(!attempts.may_try(0, "res://broken.txm.desc", 2));
}

BOOST_AUTO_TEST_CASE(ASlot_IsWrittenAtItsPlace_UndoneAsOneRecord_AndSavedAsMaterialDescReadsIt)
{
	put("textures/spec.txm.desc", R"({"__type":"texture_2d_desc","__parent":"res://base_texture.desc"})");
	const res::tag tag = put("m.desc", k_plain_material);
	edt::data_document& data = open(tag);
	edt::texture_slot_model slots(data);
	BOOST_TEST(slots.state(slot(1)).value.is_null());

	BOOST_REQUIRE(slots.set(slot(1), json::string("res://textures/spec.txm.desc")));
	BOOST_TEST((own_samplers(data) == list({ nullptr, "res://textures/spec.txm.desc" })));
	BOOST_TEST(!data.has_pending_edit());
	const std::optional<edt::history_entry> entry = data.next_undo_entry();
	BOOST_REQUIRE(entry.has_value());
	BOOST_TEST(entry->label == "Set Specular texture");
	const edt::texture_slot_state state = slots.state(slot(1));
	BOOST_TEST(state.text == "res://textures/spec.txm.desc");
	BOOST_TEST(!state.inherited);
	// The same texture again changes nothing.
	BOOST_TEST(!slots.set(slot(1), json::string("res://textures/spec.txm.desc")));

	BOOST_REQUIRE(data.undo());
	BOOST_TEST(own_samplers(data).is_null());
	BOOST_TEST(!data.is_dirty());
	BOOST_REQUIRE(data.redo());
	BOOST_TEST((own_samplers(data) == list({ nullptr, "res://textures/spec.txm.desc" })));

	const edt::save_result saved = data.save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	const std::shared_ptr<scn::material_desc> material = load<scn::material_desc>(tag);
	BOOST_REQUIRE(material);
	BOOST_REQUIRE(material->samplers_textures.size() == 2u);
	BOOST_TEST(!material->samplers_textures[0].is_valid());
	BOOST_TEST(material->samplers_textures[1].view() == "res://textures/spec.txm.desc");
}

BOOST_AUTO_TEST_CASE(AHoleStaysNull_TheEmptyEndIsTrimmed_AndClearingTheLastSlotLeavesAValidMaterial)
{
	const res::tag tag = put("m.desc", k_plain_material);
	edt::data_document& data = open(tag);
	edt::texture_slot_model slots(data);

	BOOST_REQUIRE(slots.set(slot(2), json::string("res://n.txm.desc")));
	BOOST_TEST((own_samplers(data) == list({ nullptr, nullptr, "res://n.txm.desc" })));
	BOOST_REQUIRE(slots.set(slot(0), json::string("res://d.txm.desc")));
	BOOST_TEST((own_samplers(data) == list({ "res://d.txm.desc", nullptr, "res://n.txm.desc" })));

	BOOST_REQUIRE(slots.clear(slot(2)));
	BOOST_TEST((own_samplers(data) == list({ "res://d.txm.desc" })));
	BOOST_REQUIRE(data.next_undo_entry().has_value());
	BOOST_TEST(data.next_undo_entry()->label == "Clear Normal texture");
	BOOST_TEST(!slots.clear(slot(2)));

	BOOST_REQUIRE(slots.clear(slot(0)));
	const json::value left = own_samplers(data);
	BOOST_TEST((left.is_null() || left == json::value(json::array{})));
	BOOST_TEST(data.next_undo_entry()->label == "Clear Diffuse texture");

	const edt::save_result saved = data.save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	const std::shared_ptr<scn::material_desc> material = load<scn::material_desc>(tag);
	BOOST_REQUIRE(material);
	BOOST_TEST(material->samplers_textures.empty());
}

// An inherited list shows as inherited; a change writes the material's own list, also when it empties a slot.
BOOST_AUTO_TEST_CASE(AParentsSlots_AreInherited_AndAClearWritesTheOwnList)
{
	write(lower, "parent.desc", R"({"__type":"material_desc","__version":2,"samplers":["res://p.txm.desc"]})");
	edt::data_document& data = open(put("child.desc", R"({"__type":"material_desc","__version":2,"__parent":"res://parent.desc"})"));
	edt::texture_slot_model slots(data);
	const edt::texture_slot_state inherited = slots.state(slot(0));
	BOOST_TEST(inherited.text == "res://p.txm.desc");
	BOOST_TEST(inherited.inherited);

	BOOST_REQUIRE(slots.clear(slot(0)));
	BOOST_TEST((own_samplers(data) == json::value(json::array{})));
	BOOST_TEST(slots.state(slot(0)).value.is_null());
	BOOST_TEST(!slots.state(slot(0)).inherited);

	// Undoing the clear brings the parent's list back: no own list.
	BOOST_REQUIRE(data.undo());
	BOOST_TEST(own_samplers(data).is_null());
	BOOST_TEST(slots.state(slot(0)).inherited);
	BOOST_REQUIRE(data.redo());
	BOOST_TEST((own_samplers(data) == json::value(json::array{})));

	// Setting it back to the parent's list leaves no own list.
	BOOST_REQUIRE(slots.set(slot(0), json::string("res://p.txm.desc")));
	BOOST_TEST(own_samplers(data).is_null());
}

// A field edit still pending (a widget let go of later) is a record of its own, before the slot's.
BOOST_AUTO_TEST_CASE(APendingFieldEdit_AndASlotClick_AreTwoRecords)
{
	edt::data_document& data = open(put("m.desc", k_plain_material));
	edt::texture_slot_model slots(data);
	BOOST_REQUIRE(data.edit([](json::value& shown) {
		shown.as_object()["queue"] = "transparent";
		return true;
	}));
	BOOST_REQUIRE(data.has_pending_edit());
	BOOST_REQUIRE(slots.set(slot(0), json::string("res://d.txm.desc")));
	BOOST_REQUIRE(data.next_undo_entry().has_value());
	BOOST_TEST(data.next_undo_entry()->label == "Set Diffuse texture");
	BOOST_REQUIRE(data.undo());
	BOOST_TEST(own_samplers(data).is_null());
	BOOST_TEST((data.own().at("queue") == json::value("transparent")));
	BOOST_REQUIRE(data.next_undo_entry().has_value());
	BOOST_TEST(data.next_undo_entry()->label != "Set Diffuse texture");
	BOOST_REQUIRE(data.undo());
	BOOST_TEST(!data.is_dirty());
}

BOOST_AUTO_TEST_SUITE_END()
