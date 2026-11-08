#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/desc_base.hpp>
#include <snk/resource/res_diagnostics.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/scene/scn_directional_light_desc.h>
#include <algorithm>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

// Diagnostics of desc loads (#595): each problem as resource + JSON path + reason, gathered over everything a request
// loaded, the same for every request of a cached resource.

namespace res {
	inline std::ostream& operator<<(std::ostream& out, const diagnostic& d)
	{
		return out << "{" << (d.severity == diagnostic_severity::error ? "error" : "warning") << ", " << d.kind << ", "
			<< d.resource << ", '" << d.json_path << "', " << d.reason << "}";
	}
}

namespace {

// A field typed with a base desc type: a derived desc in it is what it asks for.
struct prefab_holder_desc : desc::desc_base
{
	res::res_handle<scn::prefab_desc> prefab;

	void copy_to(desc::desc_base& other) const override { copy_to_same(*this, other); }
	void deserialize(desc::desc_system& system, const json::object& data) override
	{
		if (const auto* p = data.if_contains("prefab")) {
			prefab = system.get_field_desc<scn::prefab_desc>(*this, *p, "/prefab");
		}
	}
	void serialize(json::object&) const override {}
};

struct diagnostics_fixture : test_util::scene_env
{
	// The diagnostics of one request of `tag`, after it and what it started have settled.
	std::vector<res::diagnostic> diagnostics_of(std::string_view name)
	{
		auto handle = rs->require_sync<desc::desc_base>(tag_of(name));
		pump();
		res::diagnostics_collector collector;
		collector.collect(handle.get_control_block());
		return collector.items();
	}

	static std::string where(std::string_view name) { return std::string{ tag_of(name).view() }; }

	static const res::diagnostic* find(const std::vector<res::diagnostic>& items, std::string_view kind, std::string_view resource)
	{
		const auto it = std::ranges::find_if(items, [&](const res::diagnostic& d) { return d.kind == kind && d.resource == resource; });
		return it == items.end() ? nullptr : &*it;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ResDiagnosticsTests, diagnostics_fixture)

BOOST_AUTO_TEST_CASE(AnUnknownTypeIsAnErrorAtTheTypeField)
{
	put("unknown.desc", R"({"__type":"no_such_desc"})");
	const auto items = diagnostics_of("unknown.desc");
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_TEST((items[0].severity == res::diagnostic_severity::error));
	BOOST_TEST(items[0].kind == res::diagnostic_kind::unknown_type);
	BOOST_TEST(items[0].resource == where("unknown.desc"));
	BOOST_TEST(items[0].json_path == "/__type");
	BOOST_TEST(items[0].reason.find("no_such_desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AMissingParentIsAnErrorAtTheParentField)
{
	put("orphan.desc", R"({"__type":"prefab_desc","__parent":"memory://t/not_there.desc"})");
	const auto items = diagnostics_of("orphan.desc");
	const auto* missing = find(items, res::diagnostic_kind::missing_parent, where("orphan.desc"));
	BOOST_REQUIRE(missing);
	BOOST_TEST((missing->severity == res::diagnostic_severity::error));
	BOOST_TEST(missing->json_path == "/__parent");
	BOOST_TEST(missing->reason.find("not_there.desc") != std::string::npos);
	// and the parent's own failure, at its file
	BOOST_TEST(find(items, res::diagnostic_kind::load, where("not_there.desc")));
}

BOOST_AUTO_TEST_CASE(AParentCycleIsAnErrorOfEveryMember)
{
	put("self.desc", R"({"__type":"prefab_desc","__parent":"memory://t/self.desc"})");
	const auto self = diagnostics_of("self.desc");
	BOOST_REQUIRE_EQUAL(self.size(), 1u);
	BOOST_TEST(self[0].kind == res::diagnostic_kind::cycle);
	BOOST_TEST(self[0].json_path == "/__parent");

	put("ping.desc", R"({"__type":"prefab_desc","__parent":"memory://t/pong.desc"})");
	put("pong.desc", R"({"__type":"prefab_desc","__parent":"memory://t/ping.desc"})");
	const auto items = diagnostics_of("ping.desc");
	for (const std::string_view member : { "ping.desc", "pong.desc" }) {
		const auto* cycle = find(items, res::diagnostic_kind::cycle, where(member));
		BOOST_REQUIRE_MESSAGE(cycle, member);
		BOOST_TEST((cycle->severity == res::diagnostic_severity::error));
		BOOST_TEST(cycle->json_path == "/__parent");
		BOOST_TEST(cycle->reason.find("dependency cycle") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(AValueThatDoesNotFitTheSchemaIsAWarningAndTheLoadSucceeds)
{
	put("light.desc", R"({"__type":"directional_light_desc","diffuse":"red"})");
	auto handle = rs->require_sync<scn::directional_light_desc>(tag_of("light.desc"));
	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get()->diffuse == scn::directional_light_desc{}.diffuse);

	res::diagnostics_collector collector;
	collector.collect(handle.get_control_block());
	const auto items = collector.items();
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_TEST((items[0].severity == res::diagnostic_severity::warning));
	BOOST_TEST(items[0].kind == res::diagnostic_kind::schema);
	BOOST_TEST(items[0].resource == where("light.desc"));
	BOOST_TEST(items[0].json_path == "/diffuse");
	BOOST_TEST(collector.error_count() == 0u);
}

BOOST_AUTO_TEST_CASE(ASchemaWarningOfAnInlineDescIsAtTheOwnersFieldPath)
{
	put("sky.desc", R"({"__type":"skybox_desc","material":{"__type":"material_desc","uniforms":{"albedo":"red"}}})");
	const auto items = diagnostics_of("sky.desc");
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_TEST((items[0].severity == res::diagnostic_severity::warning));
	BOOST_TEST(items[0].kind == res::diagnostic_kind::schema);
	BOOST_TEST(items[0].resource == where("sky.desc"));
	BOOST_TEST(items[0].json_path == "/material/uniforms/albedo");
}

BOOST_AUTO_TEST_CASE(AnUnknownPrefabComponentIsAWarningAtItsPath)
{
	put("node.prefab.desc", R"({"__type":"prefab_desc","children":[
		{"name":"Child","components":{"oops":{"__type":"no_such_component_desc"}}}]})");
	auto handle = rs->require_sync<scn::prefab_desc>(tag_of("node.prefab.desc"));
	BOOST_REQUIRE(handle.is_ready());

	res::diagnostics_collector collector;
	collector.collect(handle.get_control_block());
	const auto items = collector.items();
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_TEST((items[0].severity == res::diagnostic_severity::warning));
	BOOST_TEST(items[0].kind == res::diagnostic_kind::unknown_component);
	BOOST_TEST(items[0].json_path == "/children/0/components/oops");
	BOOST_TEST(items[0].reason.find("no_such_component_desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnInlineDescReportsAtItsOwnersFileAndPath_WithoutChangingItsCacheKeyOrLoad)
{
	put("clips.desc", R"({"__type":"animation_collection_desc","clips":[
		{"__type":"animation_clip_desc","name":"idle","duration":1.0,"ticks_per_second":1.0},
		{"__type":"no_such_clip_desc","name":"walk"}]})");
	auto collection = rs->require_sync<scn::animation_collection_desc>(tag_of("clips.desc"));
	BOOST_REQUIRE(collection.is_ready());
	pump();

	res::diagnostics_collector collector;
	collector.collect(collection.get_control_block());
	const auto items = collector.items();
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_TEST(items[0].kind == res::diagnostic_kind::unknown_type);
	BOOST_TEST(items[0].resource == where("clips.desc"));
	BOOST_TEST(items[0].json_path == "/clips/1/__type");

	// The copy is still the memory:// resource it was, cached under its own tag, failed as before.
	const auto& broken = collection.get()->clips[1];
	BOOST_TEST(broken.get_tag().view().starts_with("memory://"));
	BOOST_TEST(broken.get_tag() != tag_of("clips.desc"));
	BOOST_TEST(broken.has_error());
	auto again = rs->require<scn::animation_clip_desc>(broken.get_tag());
	BOOST_TEST((again.get_control_block() == broken.get_control_block()));
	BOOST_TEST(collection.get()->clips[0].is_ready());
}

BOOST_AUTO_TEST_CASE(ADescRequiredAsAnotherType_IsAFailedHandle_AndTheBlockStaysReady)
{
	put("mat.desc", R"({"__type":"material_desc"})");
	auto level = rs->require_sync<scn::level_desc>(tag_of("mat.desc"));
	pump();
	BOOST_TEST(!level.is_ready());
	BOOST_TEST(level.has_error());
	BOOST_TEST(!level.get());
	BOOST_TEST(!level.get_sync());
	const auto text = level.error_text();
	BOOST_TEST(text.find("material_desc") != std::string::npos);
	BOOST_TEST(text.find("level_desc") != std::string::npos);

	bool called = false;
	ds::lifetime owner;
	level.then(owner, [&](scn::level_desc&) { called = true; });
	pump();
	BOOST_TEST(!called);

	res::diagnostics_collector collector;
	level.collect_into(collector);
	const auto items = collector.items();
	const auto* mismatch = find(items, res::diagnostic_kind::type_mismatch, where("mat.desc"));
	BOOST_REQUIRE(mismatch);
	BOOST_TEST((mismatch->severity == res::diagnostic_severity::error));
	BOOST_TEST(mismatch->json_path == "/__type");
	BOOST_TEST(mismatch->reason == text);

	// The cache is not touched: a request the data fits gets the same ready block.
	auto any = rs->require_sync<desc::desc_base>(tag_of("mat.desc"));
	BOOST_REQUIRE(any.is_ready());
	BOOST_TEST(any.get()->get_type() == "material_desc");
	BOOST_TEST(any.error_text().empty());
}

BOOST_AUTO_TEST_CASE(AFieldDescOfAnotherType_IsAFailedHandle_ReportedAtTheField)
{
	put("mat.desc", R"({"__type":"material_desc"})");
	put("clips.desc", R"({"__type":"animation_collection_desc","clips":["memory://t/mat.desc",{"__type":"material_desc"}]})");
	auto collection = rs->require_sync<scn::animation_collection_desc>(tag_of("clips.desc"));
	pump();
	BOOST_REQUIRE(collection.is_ready());
	const auto& clips = collection.get()->clips;
	BOOST_REQUIRE_EQUAL(clips.size(), 2u);
	for (const auto& clip : clips) {
		BOOST_TEST(clip.has_error());
		BOOST_TEST(!clip.get());
		BOOST_TEST(clip.error_text().find("animation_clip_desc") != std::string::npos);
	}

	res::diagnostics_collector collector;
	collector.collect(collection.get_control_block());
	const auto items = collector.items();
	BOOST_TEST(std::ranges::count_if(items, [](const res::diagnostic& d) { return d.kind == res::diagnostic_kind::type_mismatch; }) == 2);
	for (const std::string_view path : { "/clips/0", "/clips/1/__type" }) {
		const auto it = std::ranges::find_if(items, [&](const res::diagnostic& d) { return d.json_path == path; });
		BOOST_REQUIRE_MESSAGE(it != items.end(), path);
		BOOST_TEST((it->severity == res::diagnostic_severity::error));
		BOOST_TEST(it->kind == res::diagnostic_kind::type_mismatch);
		BOOST_TEST(it->resource == where("clips.desc"));
		BOOST_TEST(it->reason.find("material_desc") != std::string::npos);
		BOOST_TEST(it->reason.find("animation_clip_desc") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(ADependencyOfAnotherTypeIsNotGivenOut)
{
	put("mat.desc", R"({"__type":"material_desc"})");
	auto material = rs->require_sync<desc::desc_base>(tag_of("mat.desc"));
	BOOST_REQUIRE(material.is_ready());

	res::res_deps deps;
	deps.add(res::make_dep<scn::level_desc>(tag_of("mat.desc")), material.get());
	BOOST_TEST(!deps.get<scn::level_desc>(tag_of("mat.desc")));
	deps.add(res::make_dep<desc::desc_base>(tag_of("mat.desc")), material.get());
	BOOST_TEST((deps.get<desc::desc_base>(tag_of("mat.desc")) == material.get()));
}

BOOST_AUTO_TEST_CASE(ADerivedDescInABaseTypedField_IsWhatItAsksFor)
{
	descs->register_desc<prefab_holder_desc>("prefab_holder_desc");
	put("world.desc", R"({"__type":"world_desc"})");
	put("holder.desc", R"({"__type":"prefab_holder_desc","prefab":"memory://t/world.desc"})");
	auto holder = rs->require_sync<desc::desc_base>(tag_of("holder.desc"));
	pump();
	BOOST_REQUIRE(holder.is_ready());
	const auto& prefab = std::dynamic_pointer_cast<prefab_holder_desc>(holder.get())->prefab;
	BOOST_TEST(prefab.is_ready());
	BOOST_TEST(!prefab.has_error());
	BOOST_TEST(prefab.error_text().empty());

	res::diagnostics_collector collector;
	collector.collect(holder.get_control_block());
	const auto items = collector.items();
	BOOST_TEST(std::ranges::none_of(items, [](const res::diagnostic& d) { return d.kind == res::diagnostic_kind::type_mismatch; }));
}

BOOST_AUTO_TEST_CASE(TwoFieldsSharingABlock_EachReportTheirMismatch)
{
	put("mat.desc", R"({"__type":"material_desc"})");
	put("clips.desc", R"({"__type":"animation_collection_desc","clips":["memory://t/mat.desc","memory://t/mat.desc"]})");
	auto collection = rs->require_sync<scn::animation_collection_desc>(tag_of("clips.desc"));
	pump();
	BOOST_REQUIRE(collection.is_ready());
	const auto& clips = collection.get()->clips;
	BOOST_REQUIRE_EQUAL(clips.size(), 2u);
	BOOST_TEST((clips[0].get_control_block() == clips[1].get_control_block()));

	res::diagnostics_collector collector;
	collector.collect(collection.get_control_block());
	const auto items = collector.items();
	for (const std::string_view path : { "/clips/0", "/clips/1" }) {
		BOOST_TEST_MESSAGE(path);
		BOOST_TEST(std::ranges::count_if(items, [&](const res::diagnostic& d) {
			return d.kind == res::diagnostic_kind::type_mismatch && d.json_path == path;
		}) == 1);
	}
}

BOOST_AUTO_TEST_CASE(ABlockThatIsADependencyAndAField_ReportsOneMismatch)
{
	put("mat.desc", R"({"__type":"material_desc"})");
	auto material = rs->require_sync<desc::desc_base>(tag_of("mat.desc"));
	BOOST_REQUIRE(material.is_ready());

	const res::child_link field{ {}, "/level", false, &core::res::holds<scn::level_desc>, "level_desc" };
	for (const bool field_first : { false, true }) {
		auto owner = std::make_shared<core::res::res_resource_block>();
		owner->diagnostics.set_resource("memory://t/owner.desc");
		const auto child = material.get_control_block();
		if (field_first) {
			owner->diagnostics.add_child(child, field);
			owner->diagnostics.add_child(child);
		} else {
			owner->diagnostics.add_child(child);
			owner->diagnostics.add_child(child, field);
		}
		owner->set_ready(std::make_shared<res::resource_entry>());

		res::diagnostics_collector collector;
		collector.collect(owner);
		const auto items = collector.items();
		BOOST_TEST_MESSAGE(field_first);
		BOOST_REQUIRE_EQUAL(std::ranges::count_if(items, [](const res::diagnostic& d) { return d.kind == res::diagnostic_kind::type_mismatch; }), 1);
		const auto* mismatch = find(items, res::diagnostic_kind::type_mismatch, "memory://t/owner.desc");
		BOOST_REQUIRE(mismatch);
		BOOST_TEST(mismatch->json_path == "/level");
	}
}

BOOST_AUTO_TEST_CASE(AFailedOwner_DoesNotRunItsFieldChecks)
{
	put("mat.desc", R"({"__type":"material_desc"})");
	auto material = rs->require_sync<desc::desc_base>(tag_of("mat.desc"));
	BOOST_REQUIRE(material.is_ready());

	static bool checked = false;
	checked = false;
	const res::child_link field{ {}, "/level", false, [](const res::resource_entry&) { checked = true; return false; }, "level_desc" };
	auto owner = std::make_shared<core::res::res_resource_block>();
	owner->diagnostics.set_resource("memory://t/owner.desc");
	owner->diagnostics.add_child(material.get_control_block(), field);
	owner->set_error("broken");

	res::diagnostics_collector collector;
	collector.collect(owner);
	BOOST_TEST(!checked);
	const auto items = collector.items();
	BOOST_TEST(std::ranges::none_of(items, [](const res::diagnostic& d) { return d.kind == res::diagnostic_kind::type_mismatch; }));
}

BOOST_AUTO_TEST_CASE(AKeyWithSlashAndTildeIsEscapedInThePath)
{
	put("keys.prefab.desc", R"({"__type":"prefab_desc","components":{"a/b~c":{"__type":"no_such_component_desc"}}})");
	const auto items = diagnostics_of("keys.prefab.desc");
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_TEST(items[0].json_path == "/components/a~1b~0c");
}

BOOST_AUTO_TEST_CASE(OneFileKeepsAtMost64AndCountsTheRest)
{
	std::string components;
	for (int i = 0; i < 70; ++i) {
		components += std::format(R"({}"c{}":{{"__type":"no_such_component_desc"}})", i == 0 ? "" : ",", i);
	}
	put("many.prefab.desc", R"({"__type":"prefab_desc","components":{)" + components + "}}");
	const auto items = diagnostics_of("many.prefab.desc");
	BOOST_REQUIRE_EQUAL(items.size(), res::load_diagnostics::max_items + 1);
	BOOST_TEST(items.back().kind == res::diagnostic_kind::truncated);
	BOOST_TEST(items.back().reason == "6 more diagnostics");
}

BOOST_AUTO_TEST_CASE(CollectingTheSameRequestAgainAddsNothing)
{
	put("twice.desc", R"({"__type":"no_such_desc"})");
	auto handle = rs->require_sync<desc::desc_base>(tag_of("twice.desc"));
	res::diagnostics_collector collector;
	collector.collect(handle.get_control_block());
	collector.collect(handle.get_control_block());
	BOOST_TEST(collector.items().size() == 1u);
}

BOOST_AUTO_TEST_CASE(AReloadOfTheFixedFileHasNoDiagnostics)
{
	const auto tag = put("fixed.desc", R"({"__type":"no_such_desc"})");
	BOOST_TEST(diagnostics_of("fixed.desc").size() == 1u);

	put("fixed.desc", R"({"__type":"prefab_desc"})");
	rs->signal_changed(tag);
	pump();
	BOOST_TEST(diagnostics_of("fixed.desc").empty());
}

BOOST_AUTO_TEST_CASE(OwnersSharingOneInlineCopy_ReportAtTheFirstOwnersFile)
{
	// Same file name, same inline desc: desc_system names the memory copy by both, so the two share it.
	constexpr const char* text = R"({"__type":"animation_collection_desc","clips":[{"__type":"no_such_clip_desc","name":"x"}]})";
	put("first/shared.desc", text);
	put("second/shared.desc", text);

	auto first = rs->require_sync<desc::desc_base>(tag_of("first/shared.desc"));
	pump();
	auto second = rs->require_sync<desc::desc_base>(tag_of("second/shared.desc"));
	pump();

	res::diagnostics_collector of_first;
	of_first.collect(first.get_control_block());
	res::diagnostics_collector of_second;
	of_second.collect(second.get_control_block());
	BOOST_REQUIRE_EQUAL(of_first.items().size(), 1u);
	BOOST_REQUIRE_EQUAL(of_second.items().size(), 1u);
	BOOST_TEST(of_first.items()[0].resource == where("first/shared.desc"));
	// the copy belongs to the owner that loaded it first, while that one lives
	BOOST_TEST(of_second.items()[0].resource == where("first/shared.desc"));
	BOOST_TEST(of_second.items()[0].json_path == "/clips/0/__type");
}

BOOST_AUTO_TEST_CASE(ACachedFailureGivesTheSameDiagnosticsToTheNextRequest)
{
	put("again.desc", R"({"__type":"no_such_desc"})");
	const auto first = diagnostics_of("again.desc");
	const auto second = diagnostics_of("again.desc");
	BOOST_REQUIRE_EQUAL(first.size(), 1u);
	BOOST_TEST(first == second, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(TheCollectorKeepsItsLimitAndCountsTheRest)
{
	res::diagnostics_collector collector(2);
	for (const std::string_view name : { "bad_a.desc", "bad_b.desc", "bad_c.desc" }) {
		put(name, R"({"__type":"no_such_desc"})");
		collector.collect(rs->require_sync<desc::desc_base>(tag_of(name)).get_control_block());
	}
	const auto items = collector.items();
	BOOST_REQUIRE_EQUAL(items.size(), 3u);
	BOOST_TEST(items[2].kind == res::diagnostic_kind::truncated);
	BOOST_TEST(items[2].reason == "1 more diagnostics");
	BOOST_TEST(collector.error_count() == 2u);
}

BOOST_AUTO_TEST_CASE(AWarningOutsideALoadGoesNowhere)
{
	BOOST_TEST(res::load_scope::current() == nullptr);
	res::report_load_warning(res::diagnostic_kind::schema, "/x", "ignored");
	BOOST_TEST(res::json_pointer_token("a/b~c") == "a~1b~0c");
}

BOOST_AUTO_TEST_SUITE_END()
