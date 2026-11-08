#include <boost/test/unit_test.hpp>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_component_schema.h>
#include <boost/json.hpp>
#include <entt/entity/registry.hpp>
#include <glm/mat4x4.hpp>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The catalog of runtime components a live-world snapshot uses (#599, HOT_RELOAD_PLAY.md section 4.2): names in
// registration order over a parent, a component written and read back through its schema, entity references remapped
// to the restored entities, glm::mat4 and ecs::entity_ref members.

namespace catalog_test
{
	struct health
	{
		int hp = 100;
		float armor = 0.5f;
	};
	BOOST_DESCRIBE_STRUCT(health, (), (hp, armor))

	struct link
	{
		entt::entity target = entt::null;
		std::vector<entt::entity> others;
	};
	BOOST_DESCRIBE_STRUCT(link, (), (target, others))

	struct pose
	{
		glm::mat4 matrix{ 1.0f };
	};
	BOOST_DESCRIBE_STRUCT(pose, (), (matrix))

	struct anchor
	{
		ecs::entity_ref target;
	};
	BOOST_DESCRIBE_STRUCT(anchor, (), (target))

	struct marker
	{
	};
	BOOST_DESCRIBE_STRUCT(marker, (), ())

	struct cached
	{
		int value = 0;
	};

	struct score
	{
		int points = 0;
	};
	BOOST_DESCRIBE_STRUCT(score, (), (points))

	template<typename T>
	std::shared_ptr<const desc::schema_binding> schema_of(std::string_view name)
	{
		return desc::bind_described<T>(name, 1, desc::schema_kind::component);
	}

	class id_remap final : public desc::entity_remap
	{
	public:
		void map(entt::entity saved, entt::entity restored)
		{
			m_ids[static_cast<std::uint32_t>(entt::to_integral(saved))] = static_cast<std::uint32_t>(entt::to_integral(restored));
		}

		std::optional<std::uint32_t> find(std::uint32_t saved) const override
		{
			const auto it = m_ids.find(saved);
			if (it == m_ids.end()) {
				return std::nullopt;
			}
			return it->second;
		}

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> m_ids;
	};

	struct construct_observer
	{
		int* seen = nullptr;
		void on_construct(entt::registry& reg, entt::entity entity) { *seen = reg.get<health>(entity).hp; }
	};

	json::object write(const scn::component_entry& entry, const entt::registry& reg, entt::entity entity)
	{
		json::object out;
		BOOST_REQUIRE(entry.write(*entry.schema, reg, entity, out));
		return out;
	}
}

using namespace catalog_test;

BOOST_AUTO_TEST_SUITE(ComponentCatalog)

BOOST_AUTO_TEST_CASE(Catalog_ListsTheParentFirstAndRefusesTakenNamesAndTypes)
{
	scn::component_catalog base;
	BOOST_REQUIRE(base.add<health>("test.health", schema_of<health>("test.health")));
	BOOST_REQUIRE(base.add_derived<cached>("test.cached"));

	scn::component_catalog layer{ &base };
	BOOST_REQUIRE(layer.add_ctx<score>("test.score", schema_of<score>("test.score")));
	BOOST_CHECK(!layer.add<link>("test.health", schema_of<link>("test.link")));
	BOOST_CHECK(!layer.add<health>("test.other_health", schema_of<health>("test.health")));
	BOOST_CHECK(!layer.add<link>("test.link", schema_of<health>("test.health")));

	std::vector<std::string> names;
	layer.for_each([&](const scn::component_entry& entry) { names.push_back(entry.name); });
	BOOST_TEST(names == (std::vector<std::string>{ "test.health", "test.cached", "test.score" }), boost::test_tools::per_element());

	BOOST_TEST(layer.size() == 1u);
	BOOST_REQUIRE(layer.find("test.health"));
	BOOST_TEST(layer.find(entt::type_hash<score>::value())->name == "test.score");
	BOOST_TEST(layer.find("test.health")->saved());
	BOOST_TEST(!layer.find("test.cached")->saved());

	BOOST_TEST(!layer.remove("test.health"));
	BOOST_TEST(layer.remove("test.score"));
	BOOST_TEST(!layer.find("test.score"));
	BOOST_TEST(layer.add_ctx<score>("test.score", schema_of<score>("test.score")) != nullptr);
}

BOOST_AUTO_TEST_CASE(Catalog_DataRoundTripsFieldByField)
{
	scn::component_catalog catalog;
	const scn::component_entry* entry = catalog.add<health>("test.health", schema_of<health>("test.health"));
	BOOST_REQUIRE(entry);

	entt::registry source;
	const entt::entity saved = source.create();
	source.emplace<health>(saved, health{ 7, 0.25f });
	const json::object data = write(*entry, source, saved);

	entt::registry target;
	const entt::entity restored = target.create();
	int seen_on_construct = -1;
	construct_observer watch{ &seen_on_construct };
	target.on_construct<health>().connect<&construct_observer::on_construct>(watch);

	entry->read(*entry->schema, target, restored, data, {});
	BOOST_REQUIRE(target.all_of<health>(restored));
	BOOST_TEST(target.get<health>(restored).hp == 7);
	BOOST_TEST(target.get<health>(restored).armor == 0.25f);
	BOOST_TEST(seen_on_construct == 7);
}

BOOST_AUTO_TEST_CASE(Catalog_EntityReferencesAreRemappedToTheRestoredEntities)
{
	scn::component_catalog catalog;
	const scn::component_entry* entry = catalog.add<link>("test.link", schema_of<link>("test.link"));
	BOOST_REQUIRE(entry);

	entt::registry source;
	const entt::entity first = source.create();
	const entt::entity second = source.create();
	const entt::entity gone = source.create();
	source.emplace<link>(second, link{ first, { first, second, gone, entt::null } });
	source.destroy(gone);
	const json::object data = write(*entry, source, second);

	entt::registry target;
	(void)target.create(); // the restored ids differ from the saved ones
	const entt::entity first_restored = target.create();
	const entt::entity second_restored = target.create();
	id_remap remap;
	remap.map(first, first_restored);
	remap.map(second, second_restored);

	desc::read_context ctx;
	ctx.entities = &remap;
	entry->read(*entry->schema, target, second_restored, data, ctx);
	const link& restored = target.get<link>(second_restored);
	BOOST_TEST(entt::to_integral(restored.target) == entt::to_integral(first_restored));
	BOOST_REQUIRE_EQUAL(restored.others.size(), 4u);
	BOOST_TEST(entt::to_integral(restored.others[0]) == entt::to_integral(first_restored));
	BOOST_TEST(entt::to_integral(restored.others[1]) == entt::to_integral(second_restored));
	BOOST_TEST((restored.others[2] == entt::null)); // not in the snapshot
	BOOST_TEST((restored.others[3] == entt::null));
}

BOOST_AUTO_TEST_CASE(Catalog_EntityFieldIsNotReadWithoutARemap)
{
	const auto schema = schema_of<link>("test.link");
	link value;
	value.target = static_cast<entt::entity>(3);
	json::object in;
	in["target"] = 5;
	desc::deserialize(*schema, &value, in);
	BOOST_TEST(entt::to_integral(value.target) == 3u);
}

BOOST_AUTO_TEST_CASE(Catalog_Mat4AndEntityRefRoundTrip)
{
	scn::component_catalog catalog;
	const scn::component_entry* pose_entry = catalog.add<pose>("test.pose", schema_of<pose>("test.pose"));
	const scn::component_entry* anchor_entry = catalog.add<anchor>("test.anchor", schema_of<anchor>("test.anchor"));
	BOOST_REQUIRE(pose_entry && anchor_entry);

	entt::registry source;
	const entt::entity saved = source.create();
	glm::mat4 matrix{};
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			matrix[column][row] = 0.1f * static_cast<float>(column * 4 + row) + 1.0f / 3.0f;
		}
	}
	source.emplace<pose>(saved, pose{ matrix });
	source.emplace<anchor>(saved, anchor{ ecs::entity_ref{ "hud", { 4021u, 77u } } });

	entt::registry target;
	const entt::entity restored = target.create();
	pose_entry->read(*pose_entry->schema, target, restored, write(*pose_entry, source, saved), {});
	anchor_entry->read(*anchor_entry->schema, target, restored, write(*anchor_entry, source, saved), {});

	BOOST_TEST(std::memcmp(&target.get<pose>(restored).matrix, &matrix, sizeof(glm::mat4)) == 0);
	BOOST_TEST(target.get<anchor>(restored).target.context == "hud");
	BOOST_TEST(target.get<anchor>(restored).target.path == (std::vector<std::uint64_t>{ 4021u, 77u }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Catalog_EmptyComponentAndCtxResourceRoundTrip)
{
	scn::component_catalog catalog;
	const scn::component_entry* marker_entry = catalog.add<marker>("test.marker", schema_of<marker>("test.marker"));
	const scn::component_entry* score_entry = catalog.add_ctx<score>("test.score", schema_of<score>("test.score"));
	BOOST_REQUIRE(marker_entry && score_entry);

	entt::registry source;
	const entt::entity saved = source.create();
	source.emplace<marker>(saved);
	source.ctx().emplace<score>(score{ 17 });
	BOOST_TEST(marker_entry->contains(source, saved));
	BOOST_TEST(score_entry->contains(source, entt::null));

	entt::registry target;
	const entt::entity restored = target.create();
	BOOST_TEST(!score_entry->contains(target, entt::null));
	marker_entry->read(*marker_entry->schema, target, restored, write(*marker_entry, source, saved), {});
	score_entry->read(*score_entry->schema, target, entt::null, write(*score_entry, source, entt::null), {});
	BOOST_TEST(target.all_of<marker>(restored));
	BOOST_TEST(target.ctx().get<score>().points == 17);
}

BOOST_AUTO_TEST_CASE(Catalog_TypeWithoutSchemaIsKnownButNotSaved)
{
	scn::component_catalog catalog;
	const scn::component_entry* entry = catalog.add<cached>("test.cached_data", nullptr);
	BOOST_REQUIRE(entry);
	BOOST_TEST(!entry->saved());

	entt::registry reg;
	const entt::entity entity = reg.create();
	reg.emplace<cached>(entity);
	BOOST_TEST(entry->contains(reg, entity));
}

BOOST_AUTO_TEST_CASE(Catalog_RemoveForgetsTheEntry)
{
	scn::component_catalog catalog;
	BOOST_REQUIRE(catalog.add<health>("test.health", schema_of<health>("test.health")));
	BOOST_REQUIRE(catalog.add<score>("test.score", schema_of<score>("test.score")));
	BOOST_TEST(catalog.remove("test.health"));
	BOOST_TEST(!catalog.find("test.health"));
	BOOST_TEST(!catalog.find(entt::type_hash<health>::value()));
	BOOST_TEST(catalog.size() == 1u);
	std::size_t listed = 0;
	catalog.for_each([&](const scn::component_entry&) { ++listed; });
	BOOST_TEST(listed == 1u);
	catalog.clear();
	BOOST_TEST(catalog.size() == 0u);
	BOOST_TEST(!catalog.find("test.score"));
}

BOOST_AUTO_TEST_CASE(Catalog_TheParentWinsANameItGetsAfterTheLayer)
{
	scn::component_catalog base;
	scn::component_catalog layer{ &base };
	BOOST_REQUIRE(layer.add<health>("test.health", schema_of<health>("test.health")));
	BOOST_REQUIRE(base.add<health>("test.health", schema_of<health>("test.health")));

	std::vector<const scn::component_entry*> listed;
	layer.for_each([&](const scn::component_entry& entry) { listed.push_back(&entry); });
	BOOST_REQUIRE_EQUAL(listed.size(), 1u);
	BOOST_TEST(listed.front() == base.find("test.health"));
	BOOST_TEST(layer.find("test.health") == base.find("test.health"));
}

BOOST_AUTO_TEST_CASE(Catalog_AnEntityReferringToItselfComesBackToItself)
{
	scn::component_catalog catalog;
	const scn::component_entry* entry = catalog.add<link>("test.link", schema_of<link>("test.link"));
	BOOST_REQUIRE(entry);

	entt::registry source;
	const entt::entity self = source.create();
	source.emplace<link>(self, link{ self, { self } });
	const json::object data = write(*entry, source, self);

	entt::registry target;
	(void)target.create();
	const entt::entity restored = target.create();
	id_remap remap;
	remap.map(self, restored);
	desc::read_context ctx;
	ctx.entities = &remap;
	BOOST_TEST(entry->read(*entry->schema, target, restored, data, ctx));
	BOOST_TEST(entt::to_integral(target.get<link>(restored).target) == entt::to_integral(restored));
	BOOST_TEST(entt::to_integral(target.get<link>(restored).others.at(0)) == entt::to_integral(restored));
}

BOOST_AUTO_TEST_CASE(Catalog_Mat4SurvivesTextAndIsWrittenColumnByColumn)
{
	glm::mat4 matrix{};
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			matrix[column][row] = 1.0f / 7.0f + static_cast<float>(column * 10 + row);
		}
	}
	const json::value written = desc::json_codec<glm::mat4>::to_json(matrix);
	BOOST_REQUIRE_EQUAL(written.as_array().size(), 16u);
	BOOST_TEST(written.as_array()[1].to_number<double>() == static_cast<double>(matrix[0][1])); // column 0, row 1
	BOOST_TEST(written.as_array()[4].to_number<double>() == static_cast<double>(matrix[1][0])); // column 1, row 0

	const json::value parsed = json::parse(json::serialize(written));
	glm::mat4 read{};
	BOOST_REQUIRE(desc::json_codec<glm::mat4>::from_json(read, parsed));
	BOOST_TEST(std::memcmp(&read, &matrix, sizeof(glm::mat4)) == 0);
}

BOOST_AUTO_TEST_CASE(Catalog_ReadTellsAValueThatDoesNotFit)
{
	scn::component_catalog catalog;
	const scn::component_entry* entry = catalog.add<health>("test.health", schema_of<health>("test.health"));
	BOOST_REQUIRE(entry);
	entt::registry reg;
	const entt::entity entity = reg.create();
	json::object in;
	in["hp"] = "a lot";
	in["armor"] = 2.0;
	BOOST_TEST(!entry->read(*entry->schema, reg, entity, in, {}));
	BOOST_TEST(reg.get<health>(entity).hp == 100); // kept its default
	BOOST_TEST(reg.get<health>(entity).armor == 2.0f);
}

BOOST_AUTO_TEST_SUITE_END()
