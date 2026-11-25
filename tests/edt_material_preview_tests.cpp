#include <boost/test/unit_test.hpp>
#include "edt_preview_fixture.hpp"
#include "edt_data_document.h"
#include "edt_material_preview_content.h"
#include "edt_material_window_model.h"
#include "edt_preview_stage.h"
#include <snk/scene/scn_model.h>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The material window's preview content (#1196 PR 2) on a preview stage: the memory copy of the material the shape draws
// with, the shape choice, the sky material that shows nothing, and the copy given up on close. No pixels.

namespace {

namespace json = boost::json;

constexpr const char* k_plain_material = R"({"__type":"material_desc","__version":2,"queue":"opaque"})";
constexpr const char* k_sky_material = R"({"__type":"material_desc","__version":2,"shader":"res://shaders/sky.shader.desc"})";

struct material_preview_fixture : test_util::preview_env
{
	material_preview_fixture()
		: preview_env("edt_material_preview")
	{
	}

	~material_preview_fixture()
	{
		stage.reset();
		content.reset();
		model.reset();
		document.reset();
	}

	// Opens `text` as a material document with its model, its preview content and a stage, updated once (one frame shown).
	edt::preview_stage& open(std::string_view text)
	{
		document.emplace(services, layout());
		const edt::open_result opened = document->open(put("lit.desc", text));
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		model.emplace(*document, services);
		content.emplace(*document, *model, services);
		stage.emplace(services, *content);
		stage->set_targets_release(record_released());
		stage->update();
		return *stage;
	}

	// The material copy as it is in memory (null: there is none).
	std::optional<json::object> stored_material() const
	{
		const auto found = ram->files.find(content->material_tag());
		if (found == ram->files.end()) return std::nullopt;
		const std::string text(reinterpret_cast<const char*>(found->second.data()), found->second.size());
		return json::parse(text).as_object();
	}

	std::string geometry_shown()
	{
		entt::registry* reg = stage->registry();
		const entt::entity shape = shape_entity(*stage);
		if (!reg || shape == entt::null) return {};
		const auto* geometry = reg->try_get<scn::geometry_component>(shape);
		return geometry ? std::string(geometry->geom_tag.view()) : std::string{};
	}

	// What the window's Shape combo does.
	void choose(edt::preview_shape shape)
	{
		content->set_shape(shape);
		stage->sync();
	}

	std::optional<edt::data_document> document;
	std::optional<edt::material_window_model> model;
	std::optional<edt::material_preview_content> content;
	std::optional<edt::preview_stage> stage;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMaterialPreviewTests, material_preview_fixture)

BOOST_AUTO_TEST_CASE(AnEdit_RewritesTheMemoryMaterial_WhichTheShapeDrawsWith)
{
	edt::preview_stage& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(content->material_tag().view().starts_with("memory://editor/material/"));
	const std::optional<json::object> before = stored_material();
	BOOST_REQUIRE(before.has_value());
	BOOST_TEST(before->at("__type").as_string() == "material_desc");
	BOOST_TEST(!before->contains("__parent"));

	const json::array albedo{ 0.5, 0.25, 0.125, 1.0 };
	BOOST_REQUIRE(document->edit([&](json::value& value) {
		json::value& uniforms = value.as_object()["uniforms"];
		if (!uniforms.is_object()) uniforms = json::object{};
		uniforms.as_object()["albedo"] = albedo;
		return true;
	}));
	BOOST_TEST(document->is_dirty());
	shown.update();

	const std::optional<json::object> after = stored_material();
	BOOST_REQUIRE(after.has_value());
	BOOST_TEST((after->at("uniforms").as_object().at("albedo") == json::value(albedo)));
	entt::registry& reg = *shown.registry();
	const entt::entity shape = shape_entity(shown);
	BOOST_REQUIRE(shape != entt::null);
	const auto* material = reg.try_get<scn::material_desc_component>(shape);
	BOOST_REQUIRE(material != nullptr);
	BOOST_TEST(material->mlt_desc.get_tag().view() == content->material_tag().view());
	// The file is not written by the preview.
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(TheShapeChoice_SwapsTheGeometry)
{
	edt::preview_stage& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST((content->shape() == edt::preview_shape::sphere));
	BOOST_TEST(geometry_shown() == "res://shapes/sphere.geom.desc");

	choose(edt::preview_shape::cube);
	BOOST_TEST(geometry_shown() == "res://shapes/box.geom.desc");
	choose(edt::preview_shape::plane);
	BOOST_TEST(geometry_shown() == "res://shapes/plane.geom.desc");
	const entt::entity shape = shape_entity(shown);
	BOOST_REQUIRE(shape != entt::null);
	BOOST_TEST(shown.registry()->get<scn::material_desc_component>(shape).mlt_desc.get_tag().view() == content->material_tag().view());
}

BOOST_AUTO_TEST_CASE(ASkyMaterial_HasNoPreview)
{
	edt::preview_stage& shown = open(k_sky_material);

	BOOST_TEST_MESSAGE(model->shader().error);
	BOOST_TEST(model->shader().vertex.view() == "res://shaders/sky.vert");
	BOOST_TEST(edt::is_sky_material(model->shader()));
	BOOST_TEST(content->sky_material());
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(shown.registry() == nullptr);
	BOOST_TEST(!ram->files.contains(content->material_tag()));
	BOOST_TEST(released.empty());
	BOOST_TEST(shown.set_name().empty());
}

BOOST_AUTO_TEST_CASE(Close_ErasesTheMemoryMaterial)
{
	edt::preview_stage& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	const res::tag material = content->material_tag();
	BOOST_TEST(ram->files.contains(material));

	shown.close();
	BOOST_TEST(!ram->files.contains(material));

	// Shown again: the copy is written again; the owner window going erases it.
	shown.update();
	BOOST_TEST(ram->files.contains(material));
	stage.reset();
	BOOST_TEST(!ram->files.contains(material));
}

BOOST_AUTO_TEST_SUITE_END()
