#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/ecs/ecs_system.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;

namespace {

using test_util::stat_component;

struct env_fixture : test_util::scene_env
{
	res::res_handle<desc::desc_base> handle(const res::tag& tag) { return require_loaded<desc::desc_base>(tag); }

	static entt::entity child_named(entt::registry& reg, entt::entity parent, std::string_view name)
	{
		if (auto* children = reg.try_get<scn::children_component>(parent)) {
			for (auto c : children->children) {
				if (auto* n = reg.try_get<scn::name_component>(c); n && n->name == name) return c;
			}
		}
		return entt::null;
	}

	// first entity carrying a name_component equal to `name`
	static entt::entity find_named(entt::registry& reg, std::string_view name)
	{
		for (auto [e, n] : reg.view<scn::name_component>().each()) {
			if (n.name == name) return e;
		}
		return entt::null;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ScnAssemblerTests, env_fixture)

// ---------------------------------------------------------------- ecs_assembler

BOOST_AUTO_TEST_CASE(SpawnFromDesc_UnknownTypeIsWarnedAndIgnored)
{
	entt::registry reg;
	auto e = reg.create();
	auto desc = descs->create_instance("material_desc", res::tag{}); // registered desc, but no spawner
	BOOST_REQUIRE(desc);
	BOOST_CHECK_NO_THROW(assembler->spawn_from_desc(reg, e, *desc, "x"));
	BOOST_TEST(reg.orphan(e)); // nothing was attached
}

BOOST_AUTO_TEST_CASE(SpawnFromDesc_DispatchesByDescTypeAndPassesName)
{
	std::string seen_name;
	assembler->register_desc_spawner("stat_desc",
		[&](entt::registry& reg, entt::entity e, const desc::desc_base& d, std::string_view name) {
			seen_name = name;
			reg.emplace_or_replace<stat_component>(e, stat_component{ static_cast<const test_util::stat_desc&>(d).hp });
		});
	entt::registry reg;
	auto e = reg.create();
	auto d = descs->create_instance("stat_desc", res::tag{});
	static_cast<test_util::stat_desc&>(*d).hp = 42;
	assembler->spawn_from_desc(reg, e, *d, "my_stat");
	BOOST_TEST(seen_name == "my_stat");
	BOOST_TEST(reg.get<stat_component>(e).hp == 42);
}

BOOST_AUTO_TEST_CASE(UnregisterSpawner_StopsAssembly)
{
	entt::registry reg;
	auto e = reg.create();
	assembler->unregister_desc_spawner("stat_desc");
	assembler->assemble_and_apply(reg, e, "stat_desc", {}, {}, "stat");
	BOOST_TEST(!reg.all_of<stat_component>(e));
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_EmptyTypeWithoutParentDoesNothing)
{
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "", {}, {}, "c");
	BOOST_TEST(reg.orphan(e));
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_UnknownTypeDoesNothing)
{
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "no_such_desc", {}, {}, "c");
	BOOST_TEST(reg.orphan(e));
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_NoParentNoOverridesUsesTypeDefaults)
{
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "stat_desc", {}, {}, "stat");
	const auto& s = reg.get<stat_component>(e);
	BOOST_TEST(s.hp == 1);
	BOOST_TEST(s.label == "default");
	BOOST_TEST(!reg.all_of<scn::component_tracker>(e)); // nothing to track without a source desc
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_EmptyTypeTakesTypeFromParentAndTracksIt)
{
	auto tag = put("s.desc", R"({"__type":"stat_desc","hp":7,"label":"parent"})");
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "", handle(tag), {}, "stat");

	const auto& s = reg.get<stat_component>(e);
	BOOST_TEST(s.hp == 7);
	BOOST_TEST(s.label == "parent");
	const auto& info = reg.get<scn::component_tracker>(e).tracked_components.at("stat");
	BOOST_TEST(info.type_name == "stat_desc");
	BOOST_TEST(info.source_desc == tag);
	BOOST_TEST(info.local_overrides.empty());
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_OverridesLayerOverParentCopy)
{
	auto tag = put("s.desc", R"({"__type":"stat_desc","hp":7,"label":"parent"})");
	auto parent = handle(tag);
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "stat_desc", parent, json::parse(R"({"hp":99})").as_object(), "stat");

	const auto& s = reg.get<stat_component>(e);
	BOOST_TEST(s.hp == 99);
	BOOST_TEST(s.label == "parent");
	// the shared parent desc itself is NOT modified by the override
	BOOST_TEST(static_cast<const test_util::stat_desc&>(*parent.get()).hp == 7);
	BOOST_TEST(reg.get<scn::component_tracker>(e).tracked_components.at("stat").local_overrides.at("hp").as_int64() == 99);
}

// The temporary desc of an override has no tag, so an inline desc_ref in the merged data is refused with a warning; the
// parent's handle was copied into it first, so the component still gets the parent's material (#131-d).
BOOST_AUTO_TEST_CASE(AssembleAndApply_SkyboxWithAnInlineMaterialKeepsItUnderAnUnrelatedOverride)
{
	auto tag = put("sky.desc", R"({"__type":"skybox_desc","material":{"__type":"material_desc","queue":"transparent",
		"shader_vertex":"res://shaders/sky.vert"}})");
	auto parent = handle(tag);
	auto parent_sky = std::dynamic_pointer_cast<scn::skybox_desc>(parent.get_sync());
	BOOST_REQUIRE(parent_sky);
	BOOST_REQUIRE(parent_sky->material.is_valid());
	const res::tag inline_tag = parent_sky->material.get_tag();
	BOOST_TEST(inline_tag.view().starts_with("memory://overrides/"));

	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "skybox_desc", parent, json::parse(R"({"exposure":2})").as_object(), "sky");
	for (const bool hot_reload : { false, true }) {
		BOOST_TEST_CONTEXT((hot_reload ? "hot reload" : "spawn")) {
			if (hot_reload) {
				assembler->assemble_and_apply(reg, e, "skybox_desc", parent, json::parse(R"({"exposure":2})").as_object(), "sky", true);
			}
			const auto* component = reg.try_get<scn::material_desc_component>(e);
			BOOST_REQUIRE(component);
			BOOST_REQUIRE(component->mlt_desc.is_valid());
			BOOST_TEST((component->mlt_desc.get_tag() == inline_tag));
			pump();
			const auto material = component->mlt_desc.get_sync();
			BOOST_REQUIRE(material);
			BOOST_TEST((material->queue == scn::pass_queue::TRANSPARENT));
			BOOST_TEST(material->vertex_shader.string() == "res://shaders/sky.vert");
		}
	}
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_StructuralSpawnersAreNotTracked)
{
	auto tag = put("p.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":3}}})");
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "", handle(tag), {}, "p");
	BOOST_TEST(reg.get<stat_component>(e).hp == 3);          // prefab content was spawned
	BOOST_TEST(reg.all_of<scn::prefab_instance>(e));
	// only the inner (non-structural) component with a parent would be tracked; "p" itself is not
	if (auto* tr = reg.try_get<scn::component_tracker>(e)) {
		BOOST_TEST(!tr->tracked_components.contains("p"));
	}
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_HotReloadUsesPatcherAndKeepsTracking)
{
	auto tag = put("s.desc", R"({"__type":"stat_desc","hp":1})");
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "", handle(tag), {}, "stat");
	BOOST_REQUIRE(reg.get<stat_component>(e).patch_count == 0);

	put("s.desc", R"({"__type":"stat_desc","hp":2})");
	rs->signal_changed(tag);
	pump();
	assembler->assemble_and_apply(reg, e, "stat_desc", handle(tag), {}, "stat", /*is_hot_reload*/ true);

	BOOST_TEST(reg.get<stat_component>(e).hp == 2);
	BOOST_TEST(reg.get<stat_component>(e).patch_count == 1); // patch path, not a re-spawn
}

BOOST_AUTO_TEST_CASE(AssembleAndApply_HotReloadFallsBackToSpawnWhenNoPatcher)
{
	entt::registry reg;
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "object_desc", {}, {}, "o", /*is_hot_reload*/ true);
	BOOST_TEST(reg.all_of<scn::object_component>(e));
}

BOOST_AUTO_TEST_CASE(TriggerHotReloadForComponent_PatchesOnlyEntitiesTrackingThatDesc_KeepingLocalOverrides)
{
	auto tag_a = put("a.desc", R"({"__type":"stat_desc","hp":1,"label":"A"})");
	auto tag_b = put("b.desc", R"({"__type":"stat_desc","hp":1,"label":"B"})");
	entt::registry reg;
	auto with_override = reg.create();
	auto plain = reg.create();
	auto other = reg.create();
	assembler->assemble_and_apply(reg, with_override, "stat_desc", handle(tag_a), json::parse(R"({"hp":50})").as_object(), "stat");
	assembler->assemble_and_apply(reg, plain, "", handle(tag_a), {}, "stat");
	assembler->assemble_and_apply(reg, other, "", handle(tag_b), {}, "stat");

	put("a.desc", R"({"__type":"stat_desc","hp":2,"label":"A2"})");
	rs->signal_changed(tag_a);
	pump();
	assembler->trigger_hot_reload_for_component(reg, handle(tag_a));

	BOOST_TEST(reg.get<stat_component>(with_override).hp == 50);       // local override survives
	BOOST_TEST(reg.get<stat_component>(with_override).label == "A2");   // non-overridden field follows the desc
	BOOST_TEST(reg.get<stat_component>(plain).hp == 2);
	BOOST_TEST(reg.get<stat_component>(plain).label == "A2");
	BOOST_TEST(reg.get<stat_component>(other).label == "B");            // different source: untouched
	BOOST_TEST(reg.get<stat_component>(other).patch_count == 0);
}

// ---------------------------------------------------------------- hot_reload_manager

BOOST_AUTO_TEST_CASE(HotReloadManager_ReloadsPrefabInstancesOnTickAfterResourceChange)
{
	auto tag = put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	entt::registry reg;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(reg);

	auto e = reg.create();
	scn::assemble_prefab(*assembler, reg, e, *load<scn::prefab_desc>(tag), "inst", tag);
	BOOST_REQUIRE(reg.get<stat_component>(e).hp == 1);

	mgr.tick();   // nothing changed yet
	BOOST_TEST(reg.get<stat_component>(e).hp == 1);

	put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":8}}})");
	rs->signal_changed(tag);
	pump();
	BOOST_TEST(reg.get<stat_component>(e).hp == 1); // reload is deferred to tick()
	mgr.tick();
	BOOST_TEST(reg.get<stat_component>(e).hp == 8);
}

BOOST_AUTO_TEST_CASE(HotReloadManager_DetachedRegistryIsNotReloaded)
{
	auto tag = put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	entt::registry attached, detached;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(attached);
	mgr.attach_registry(detached);
	auto ea = attached.create();
	auto ed = detached.create();
	scn::assemble_prefab(*assembler, attached, ea, *load<scn::prefab_desc>(tag), "i", tag);
	scn::assemble_prefab(*assembler, detached, ed, *load<scn::prefab_desc>(tag), "i", tag);
	mgr.detach_registry(detached);

	put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":5}}})");
	rs->signal_changed(tag);
	pump();
	mgr.tick();

	BOOST_TEST(attached.get<stat_component>(ea).hp == 5);
	BOOST_TEST(detached.get<stat_component>(ed).hp == 1);
}

BOOST_AUTO_TEST_CASE(HotReloadManager_ExternalWatchReplacesInternalReloadAndDeduplicatesSignals)
{
	auto tag = put("ext.desc", R"({"__type":"stat_desc","hp":1})");
	entt::registry reg;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(reg); // tick() is a no-op without any attached registry

	int calls = 0;
	mgr.watch_resource(tag, [&](const res::tag& t) { ++calls; BOOST_TEST(t == tag); });

	rs->signal_changed(tag);
	rs->signal_changed(tag); // queued twice, delivered once
	pump();
	BOOST_TEST(calls == 0);
	mgr.tick();
	BOOST_TEST(calls == 1);
	mgr.tick();
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(HotReloadManager_UnwatchedNonPrefabDescDoesNotAbort)
{
	auto tag = put("ext2.desc", R"({"__type":"stat_desc","hp":1})");
	entt::registry reg;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(reg);
	int calls = 0;
	mgr.watch_resource(tag, [&](const res::tag&) { ++calls; });
	mgr.unwatch_resource(tag);
	rs->signal_changed(tag);
	pump();
	mgr.tick();
	BOOST_TEST(calls == 0);
}

BOOST_AUTO_TEST_CASE(HotReloadManager_ReloadsEntitiesUsingAChangedComponentDesc)
{
	auto tag = put("c.desc", R"({"__type":"stat_desc","hp":1,"label":"v1"})");
	entt::registry reg;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(reg);

	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "", handle(tag), {}, "stat");
	BOOST_REQUIRE(reg.get<stat_component>(e).label == "v1");

	put("c.desc", R"({"__type":"stat_desc","hp":1,"label":"v2"})");
	rs->signal_changed(tag);
	pump();
	mgr.tick();

	BOOST_TEST(reg.get<stat_component>(e).label == "v2");
}

// A file deleted or moved away (a branch switch, a save that renames a backup) is no failed reload: no ERROR, the
// registry keeps what it has (#515).
BOOST_AUTO_TEST_CASE(HotReloadManager_ADeletedComponentDesc_KeepsTheRegistryWithoutAnError)
{
	auto tag = put("gone.desc", R"({"__type":"stat_desc","hp":4,"label":"kept"})");
	entt::registry reg;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(reg);
	auto e = reg.create();
	assembler->assemble_and_apply(reg, e, "", handle(tag), {}, "stat");
	BOOST_REQUIRE(reg.get<stat_component>(e).label == "kept");

	std::vector<std::string> errors;
	const auto sink = engine::add_log_sink([&errors](std::string_view category, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::error) errors.push_back(std::format("{}: {}", category, message));
	});
	BOOST_REQUIRE(ram->erase(tag));
	rs->signal_changed(tag);
	pump();
	mgr.tick();

	for (const auto& error : errors) BOOST_TEST_MESSAGE(error);
	BOOST_TEST(errors.empty());
	BOOST_TEST(reg.get<stat_component>(e).hp == 4);
	BOOST_TEST(reg.get<stat_component>(e).label == "kept");
}

BOOST_AUTO_TEST_CASE(HotReloadManager_DestructionUnhooksRegistryAndWatchers)
{
	auto tag = put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	entt::registry reg;
	auto e = reg.create();
	{
		scn::hot_reload_manager mgr(*assembler, *rs);
		mgr.attach_registry(reg);
		scn::assemble_prefab(*assembler, reg, e, *load<scn::prefab_desc>(tag), "i", tag);
	}
	// manager gone: signalling and mutating the registry must not call into a dead manager
	BOOST_CHECK_NO_THROW(rs->signal_changed(tag));
	BOOST_CHECK_NO_THROW(pump());
	BOOST_CHECK_NO_THROW(reg.emplace_or_replace<scn::prefab_instance>(e, scn::prefab_instance{ tag, {} }));
}

BOOST_AUTO_TEST_CASE(HotReloadManager_ATagSharedByTwoRegistriesStaysWatchedUntilBothAreDetached)
{
	auto tag = put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	const auto prefab = load<scn::prefab_desc>(tag);
	entt::registry first, second;
	scn::hot_reload_manager mgr(*assembler, *rs);
	BOOST_TEST(mgr.watched_count() == 0u);
	const std::size_t system_watched = rs->watched_tag_count();
	mgr.attach_registry(first);
	mgr.attach_registry(second);
	const auto e_first = first.create();
	const auto e_second = second.create();
	scn::assemble_prefab(*assembler, first, e_first, *prefab, "i", tag);
	const std::size_t watched = mgr.watched_count();
	BOOST_TEST(watched >= 1u);
	scn::assemble_prefab(*assembler, second, e_second, *prefab, "i", tag);
	BOOST_TEST(mgr.watched_count() == watched); // the same tags, watched once

	mgr.detach_registry(first);
	BOOST_TEST(mgr.watched_count() == watched); // still used by the second registry
	mgr.detach_registry(second);
	BOOST_TEST(mgr.watched_count() == 0u);
	BOOST_TEST(rs->watched_tag_count() == system_watched);
	mgr.detach_registry(second); // not attached: nothing to release twice
	BOOST_TEST(mgr.watched_count() == 0u);
}

BOOST_AUTO_TEST_CASE(HotReloadManager_ATagWithACallbackAndARegistryStaysWatchedUntilBothLeave)
{
	auto tag = put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	const auto prefab = load<scn::prefab_desc>(tag);
	for (const bool callback_leaves_first : { true, false }) {
		BOOST_TEST_CONTEXT("callback leaves first: " << callback_leaves_first) {
			entt::registry reg;
			scn::hot_reload_manager mgr(*assembler, *rs);
			mgr.attach_registry(reg);
			const auto e = reg.create();
			scn::assemble_prefab(*assembler, reg, e, *prefab, "i", tag);
			const std::size_t with_registry = mgr.watched_count();
			int calls = 0;
			mgr.watch_resource(tag, [&calls](const res::tag&) { ++calls; });
			BOOST_TEST(mgr.watched_count() == with_registry); // the registry already watches this tag

			if (callback_leaves_first) {
				mgr.unwatch_resource(tag);
			} else {
				mgr.detach_registry(reg);
			}
			BOOST_TEST(mgr.watched_count() == with_registry);

			rs->signal_changed(tag);
			pump();
			mgr.tick();
			BOOST_TEST(calls == (callback_leaves_first ? 0 : 1)); // the one left still hears the change

			if (callback_leaves_first) {
				mgr.detach_registry(reg);
			} else {
				mgr.unwatch_resource(tag);
			}
			BOOST_TEST(mgr.watched_count() == 0u);
		}
	}
}

BOOST_AUTO_TEST_CASE(HotReloadManager_UnwatchResourceStopsWatchingAnExternalTag)
{
	auto tag = put("ext3.desc", R"({"__type":"stat_desc","hp":1})");
	entt::registry reg;
	scn::hot_reload_manager mgr(*assembler, *rs);
	mgr.attach_registry(reg);
	const std::size_t baseline = mgr.watched_count();

	mgr.watch_resource(tag, [](const res::tag&) {});
	BOOST_TEST(mgr.watched_count() == baseline + 1);
	int calls = 0;
	mgr.watch_resource(tag, [&calls](const res::tag&) { ++calls; }); // replaces the callback, the tag counts once
	BOOST_TEST(mgr.watched_count() == baseline + 1);

	mgr.unwatch_resource(tag);
	BOOST_TEST(mgr.watched_count() == baseline);
	mgr.unwatch_resource(tag); // twice: nothing left to release
	BOOST_TEST(mgr.watched_count() == baseline);

	rs->signal_changed(tag);
	pump();
	mgr.tick();
	BOOST_TEST(calls == 0);
}

BOOST_AUTO_TEST_CASE(HotReloadManager_AChangeQueuedBeforeTheLastWatcherLeftIsNotReloaded)
{
	auto tag = put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	const auto prefab = load<scn::prefab_desc>(tag);
	entt::registry tracked, untracked;
	scn::hot_reload_manager mgr(*assembler, *rs);
	// Assembled before it is attached: the manager never sees its tag, but would reload it with any queued change.
	const auto e_untracked = untracked.create();
	scn::assemble_prefab(*assembler, untracked, e_untracked, *prefab, "i", tag);
	mgr.attach_registry(tracked);
	mgr.attach_registry(untracked);
	const auto e_tracked = tracked.create();
	scn::assemble_prefab(*assembler, tracked, e_tracked, *prefab, "i", tag);
	BOOST_REQUIRE(mgr.watched_count() >= 1u);

	put("m.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":5}}})");
	rs->signal_changed(tag);
	pump(); // queued in the manager
	mgr.detach_registry(tracked);
	BOOST_TEST(mgr.watched_count() == 0u);
	mgr.tick();

	BOOST_TEST(untracked.get<stat_component>(e_untracked).hp == 1);
	BOOST_TEST(tracked.get<stat_component>(e_tracked).hp == 1);
}

// ---------------------------------------------------------------- world_desc / level_desc / level

BOOST_AUTO_TEST_CASE(WorldDesc_ParsesNameSystemsAndPrefabContent)
{
	auto tag = put("w.desc", R"({"__type":"world_desc","name":"w","systems":["a::s1","b::s2"],
		"components":{"anchor":{"__type":"anchor_desc"}},"children":{"Sun":{"__type":"directional_light_desc"}}})");
	auto w = load<scn::world_desc>(tag);
	BOOST_REQUIRE(w);
	BOOST_TEST(w->get_name() == "w");
	BOOST_TEST((w->get_systems() == std::vector<std::string>{ "a::s1", "b::s2" }));
	BOOST_TEST(w->get_root().children.size() == 1u);
	BOOST_TEST(w->get_root().components.contains("anchor"));
}

BOOST_AUTO_TEST_CASE(WorldDesc_SerializeKeepsTypeNameSystemsAndTree)
{
	auto tag = put("w.desc", R"({"__type":"world_desc","name":"w","systems":["s1"],
		"children":{"c":{"transform":{"position":[1,0,0]},"components":{"stat":{"__type":"stat_desc","hp":3}}}}})");
	auto w = load<scn::world_desc>(tag);
	json::object out;
	w->serialize(out);

	BOOST_TEST(out.at("__type").as_string() == "world_desc");
	BOOST_TEST(out.at("name").as_string() == "w");
	BOOST_TEST(out.at("systems").as_array().size() == 1u);
	const auto& c = out.at("children").as_array().at(0);
	BOOST_TEST(c.at("name").as_string() == "c");
	BOOST_TEST(c.at("components").at("stat").at("hp").to_number<int>() == 3);

	// feed the output back through the loader
	auto tag2 = put("w2.desc", json::serialize(out));
	auto w2 = load<scn::world_desc>(tag2);
	BOOST_REQUIRE(w2);
	BOOST_TEST(w2->get_name() == "w");
	BOOST_TEST(w2->get_root().children[0].name == "c");
}

BOOST_AUTO_TEST_CASE(LevelDesc_LoadsInlineWorldsAndBuildJsonRoundTrips)
{
	auto tag = put("lv.desc", R"({"__type":"level_desc","worlds":[
		{"__type":"world_desc","name":"w0","systems":["x"]},
		{"__type":"world_desc","name":"w1","systems":[]}]})");
	auto lv = load<scn::level_desc>(tag);
	BOOST_REQUIRE(lv);
	BOOST_REQUIRE(lv->get_worlds().size() == 2u);
	BOOST_TEST(lv->get_worlds()[0]->get_name() == "w0");
	BOOST_TEST(lv->get_worlds()[1]->get_name() == "w1");
	BOOST_TEST((lv->get_worlds()[0]->get_systems() == std::vector<std::string>{ "x" }));

	// build_json from value-typed worlds, then load it again
	std::vector<scn::world_desc> worlds(2);
	worlds[0].deserialize(*descs, json::parse(R"({"name":"a","systems":["s"]})").as_object());
	worlds[1].deserialize(*descs, json::parse(R"({"name":"b"})").as_object());
	auto built = scn::level_desc::build_json("L", worlds, *descs);
	BOOST_TEST(built.at("__type").as_string() == "level_desc");
	BOOST_TEST(built.at("name").as_string() == "L");
	BOOST_TEST(built.at("worlds").as_array().size() == 2u);

	auto lv2 = load<scn::level_desc>(put("lv2.desc", json::serialize(built)));
	BOOST_REQUIRE(lv2);
	BOOST_REQUIRE(lv2->get_worlds().size() == 2u);
	BOOST_TEST(lv2->get_worlds()[0]->get_name() == "a");
	BOOST_TEST(lv2->get_worlds()[1]->get_name() == "b");
}

BOOST_AUTO_TEST_CASE(LevelDesc_SameInlineWorldContentSharesOneCachedDesc)
{
	// get_field_desc stores inline objects under a content-hash tag, so identical worlds reuse it
	auto lv = load<scn::level_desc>(put("lv.desc", R"({"__type":"level_desc","worlds":[
		{"__type":"world_desc","name":"same"},{"__type":"world_desc","name":"same"}]})"));
	BOOST_REQUIRE(lv);
	BOOST_REQUIRE(lv->get_worlds().size() == 2u);
	BOOST_TEST(lv->get_worlds()[0].get_tag() == lv->get_worlds()[1].get_tag());
}

struct level_fixture : env_fixture
{
	level_fixture()
	{
		manager = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
	}

	std::shared_ptr<scn::level_desc> load_level()
	{
		return load<scn::level_desc>(put("lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[
			{"__type":"world_desc","name":"w0","systems":[],
			 "components":{"anchor":{"__type":"anchor_desc"}},
			 "children":{"Sun":{"__type":"directional_light_desc"}}},
			{"__type":"world_desc","name":"w1","systems":[],
			 "children":{"c":{"components":{"stat":{"__type":"stat_desc","hp":9}}}}}]})"));
	}

	std::shared_ptr<scn::hot_reload_manager> manager;
	ecs::system_factory sf;
};

BOOST_FIXTURE_TEST_CASE(Level_LoadFromDescBuildsOneRegistryPerWorldInOrder, level_fixture)
{
	auto lv_desc = load_level();
	BOOST_REQUIRE(lv_desc);
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);

	BOOST_TEST(lvl.size() == 2u);
	BOOST_TEST(lvl.has("w0"));
	BOOST_TEST(lvl.has("w1"));
	BOOST_TEST(!lvl.has("w2"));
	BOOST_TEST(lvl.tag() == tag_of("lv.desc"));
	const std::uint32_t first_world_id = 0;
	BOOST_TEST(lvl.get(first_world_id).get_name() == "w0"); // worlds get ids in file order; a literal 0 (even uint32_t{0}, under GCC -Wpedantic) would also convert to string_view
	BOOST_TEST(lvl.get(1u).get_name() == "w1");
	BOOST_TEST(lvl.acquire_free_id() == 2u);

	auto& r0 = lvl.get("w0").state();
	auto& r1 = lvl.get("w1").state();
	BOOST_TEST(lvl.get("w0").get_id() == 0u);
	BOOST_TEST(lvl.get("w1").get_id() == 1u);

	// world 0: root has the anchor and a Sun child; world 1 is independent
	auto root0 = find_named(r0, "w0");
	BOOST_REQUIRE(r0.valid(root0));
	BOOST_TEST(r0.all_of<scn::scene_anchor_component>(root0));
	BOOST_TEST(r0.all_of<scn::directional_light>(child_named(r0, root0, "Sun")));
	auto c = find_named(r1, "c");
	BOOST_REQUIRE(r1.valid(c));
	BOOST_TEST(r1.get<stat_component>(c).hp == 9);
	BOOST_TEST((find_named(r0, "c") == entt::null));
}

BOOST_FIXTURE_TEST_CASE(Level_HotReloadTickRefreshesAttachedWorlds, level_fixture)
{
	auto tag = put("lvl.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	auto lv_desc = load_level();
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);

	auto& reg = lvl.get("w1").state();
	auto e = reg.create();
	scn::assemble_prefab(*assembler, reg, e, *load<scn::prefab_desc>(tag), "inst", tag);

	put("lvl.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":6}}})");
	rs->signal_changed(tag);
	pump();
	manager->tick(); // context_manager::update does this after the sets
	BOOST_TEST(reg.get<stat_component>(e).hp == 6);
}

BOOST_FIXTURE_TEST_CASE(Level_ReloadWorldRebuildsOnlyThatWorld, level_fixture)
{
	auto lv_desc = load_level();
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);
	auto before_w0 = find_named(lvl.get("w0").state(), "w0");

	auto new_w1 = load<scn::world_desc>(put("w1v2.desc", R"({"__type":"world_desc","name":"w1","systems":[],
		"children":{"d":{"components":{"stat":{"__type":"stat_desc","hp":2}}}}})"));
	lvl.reload_context("w1", *new_w1, sf, *assembler);

	auto& r1 = lvl.get("w1").state();
	BOOST_TEST(lvl.size() == 2u);
	BOOST_TEST(lvl.get("w1").get_id() == 1u);       // id preserved
	BOOST_TEST((find_named(r1, "c") == entt::null));        // old content gone
	BOOST_TEST(r1.valid(find_named(r1, "d")));            // new content present
	BOOST_TEST((find_named(lvl.get("w0").state(), "w0") == before_w0)); // other world untouched
}

BOOST_FIXTURE_TEST_CASE(Level_InputLayerOfTheDescIsAppliedByLoadReloadAndSystemsReload, level_fixture)
{
	auto lv_desc = load<scn::level_desc>(put("layers.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":[]},
		{"__type":"world_desc","name":"ui","systems":[],"input_layer":1}]})"));
	BOOST_REQUIRE(lv_desc);
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);
	BOOST_TEST(lvl.get("w0").input_layer() == 0);
	BOOST_TEST(lvl.get("ui").input_layer() == 1);

	auto new_ui = load<scn::world_desc>(put("ui2.desc", R"({"__type":"world_desc","name":"ui","systems":[],"input_layer":3})"));
	lvl.reload_context("ui", *new_ui, sf, *assembler);
	BOOST_TEST(lvl.get("ui").input_layer() == 3);

	auto changed = load<scn::level_desc>(put("layers2.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":[],"input_layer":2},
		{"__type":"world_desc","name":"ui","systems":[]}]})"));
	lvl.load_systems_from_desc(*changed, sf);
	BOOST_TEST(lvl.get("w0").input_layer() == 2);
	BOOST_TEST(lvl.get("ui").input_layer() == 0);
}

BOOST_FIXTURE_TEST_CASE(Level_DescIndexFollowsTheFileAndWorldsMadeLaterComeAfterAll_NeverReused, level_fixture)
{
	auto lv_desc = load<scn::level_desc>(put("order.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"first","systems":[]},
		{"__type":"world_desc","name":"second","systems":[]},
		{"__type":"world_desc","name":"third","systems":[]}]})"));
	BOOST_REQUIRE(lv_desc);
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);
	BOOST_TEST(lvl.get("first").desc_index() < lvl.get("second").desc_index());
	BOOST_TEST(lvl.get("second").desc_index() < lvl.get("third").desc_index());

	lvl.destroy_context("second");
	lvl.create_context("made_while_running", lvl.acquire_free_id()); // takes the freed id 1, not the freed index
	BOOST_TEST(lvl.get("made_while_running").get_id() == 1u);
	BOOST_TEST(lvl.get("made_while_running").desc_index() > lvl.get("third").desc_index());

	lvl.create_context("and_another", lvl.acquire_free_id());
	BOOST_TEST(lvl.get("and_another").desc_index() > lvl.get("made_while_running").desc_index());
}

BOOST_FIXTURE_TEST_CASE(Level_ReloadingAWorldKeepsItsDescIndex_AndTheInputStackDefaultsFollowTheDesc, level_fixture)
{
	auto lv_desc = load<scn::level_desc>(put("keep.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":[]},
		{"__type":"world_desc","name":"ui","systems":[],"input_stack":{"block_below":true,"keys":["ESCAPE"]}}]})"));
	BOOST_REQUIRE(lv_desc);
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);
	const uint32_t index = lvl.get("ui").desc_index();
	BOOST_REQUIRE(lvl.get("ui").input_stack().has_value());
	BOOST_TEST(lvl.get("ui").input_stack()->block_below);
	BOOST_TEST(!lvl.get("w0").input_stack().has_value());

	auto plain = load<scn::world_desc>(put("ui_plain.desc", R"({"__type":"world_desc","name":"ui","systems":[]})"));
	lvl.reload_context("ui", *plain, sf, *assembler);
	BOOST_TEST(lvl.get("ui").desc_index() == index);
	BOOST_TEST(!lvl.get("ui").input_stack().has_value());
}

BOOST_FIXTURE_TEST_CASE(Level_RemoveWorldFreesItsIdAndClearEmptiesEverything, level_fixture)
{
	auto lv_desc = load_level();
	scn::context_set lvl("level", manager);
	lvl.load_from_desc(*lv_desc, sf, *assembler);

	lvl.destroy_context("w0");
	BOOST_TEST(lvl.size() == 1u);
	BOOST_TEST(!lvl.has("w0"));
	BOOST_TEST(lvl.acquire_free_id() == 0u); // gap reused

	lvl.clear();
	BOOST_TEST(lvl.size() == 0u);
	BOOST_TEST(!lvl.has("w1"));
	BOOST_TEST(lvl.acquire_free_id() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
