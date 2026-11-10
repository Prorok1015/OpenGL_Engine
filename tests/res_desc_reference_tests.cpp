#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/res_diagnostics.h>
#include <snk/scene/adapters/scn_worldwrap_adapter.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_skybox_desc.h>
#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// #697: what a desc may refer to. res://, engine://, memory:// and the desc's own protocol; never user://, cache://,
// code://, edt:// or an unknown scheme, at any place a desc reads a tag, and nothing of those is ever read.

namespace json = boost::json;

namespace {

class counting_resolver : public test_util::ram_resolver
{
public:
	using test_util::ram_resolver::ram_resolver;

	async_raw_data resolve(const res::tag& tag) const override
	{
		++resolves;
		return test_util::ram_resolver::resolve(tag);
	}

	mutable std::size_t resolves = 0;
};

constexpr std::string_view k_closed[] = { "user", "cache", "code", "edt", "x-y" };

struct reference_fixture : test_util::scene_env
{
	reference_fixture()
	{
		rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
		for (const std::string_view protocol : k_closed) {
			auto& mount = rs->registrate_resolver<counting_resolver>(protocol);
			for (const std::string_view name : { "x.frag", "m.desc", "base.desc", "b.png", "w.desc" }) {
				const res::tag tag{ std::format("{}://{}", protocol, name) };
				const std::string content = name.ends_with(".desc")
					? (name == "w.desc" ? R"({"__type":"world_desc","name":"secret"})" : R"({"__type":"material_desc","queue":"transparent"})")
					: "secret";
				mount.store(tag, test_util::to_bytes(content));
			}
			mounts[std::string{ protocol }] = &mount;
		}
	}

	std::size_t reads() const
	{
		std::size_t total = 0;
		for (const auto& [name, mount] : mounts) total += mount->resolves;
		return total;
	}

	template<class T>
	static std::vector<res::diagnostic> diagnostics_of(const res::res_handle<T>& handle)
	{
		res::diagnostics_collector collector;
		collector.collect(handle.get_control_block());
		return collector.items();
	}

	static const res::diagnostic* at(const std::vector<res::diagnostic>& items, std::string_view kind, std::string_view path)
	{
		const auto it = std::ranges::find_if(items, [&](const res::diagnostic& d) { return d.kind == kind && d.json_path == path; });
		return it == items.end() ? nullptr : &*it;
	}

	std::map<std::string, counting_resolver*> mounts;
};

}

BOOST_FIXTURE_TEST_SUITE(ResDescReferenceTests, reference_fixture)

BOOST_AUTO_TEST_CASE(ReferenceProblem_AllowsResEngineMemoryAndTheOwnersProtocolOnly)
{
	for (const std::string_view text : { "res://a.png", "engine://a.png", "memory://a.png", "", "a.png" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(res::reference_problem(res::tag{ text }).empty());
		}
	}
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const res::tag tag{ std::format("{}://a.png", protocol) };
			const std::string why = res::reference_problem(tag);
			BOOST_TEST(why.find(std::format("not '{}://'", protocol)) != std::string::npos);
			BOOST_TEST(res::reference_problem(tag, protocol).empty());
			BOOST_TEST(!res::reference_problem(tag, "res").empty());
		}
	}
	BOOST_TEST(!res::reference_problem(res::tag{ "RES://a.png" }).empty());
}

BOOST_AUTO_TEST_CASE(ATagField_IsEmptied_WithAWarningAtTheField)
{
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const res::tag tag = put(std::format("tag_{}.desc", protocol),
				std::format(R"json({{"__type":"material_desc","shader_fragment":"{}://x.frag","shader_vertex":"engine://v.vert"}})json", protocol));
			const auto handle = require_loaded<scn::material_desc>(tag);
			BOOST_REQUIRE(handle.is_ready());
			BOOST_TEST(!handle.get()->fragment_shader.is_valid());
			BOOST_TEST(handle.get()->vertex_shader.string() == "engine://v.vert");
			const auto items = diagnostics_of(handle);
			const res::diagnostic* refused = at(items, res::diagnostic_kind::reference, "/shader_fragment");
			BOOST_REQUIRE(refused);
			BOOST_TEST((refused->severity == res::diagnostic_severity::warning));
			BOOST_TEST(refused->reason.find(std::format("not '{}://'", protocol)) != std::string::npos);
		}
	}
	BOOST_TEST(reads() == 0u);
}

BOOST_AUTO_TEST_CASE(ADescField_LoadsNothing_TheHandleFails_WithAWarningAtTheField)
{
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const res::tag tag = put(std::format("sky_{}.desc", protocol),
				std::format(R"json({{"__type":"skybox_desc","material":"{}://m.desc"}})json", protocol));
			const auto handle = require_loaded<scn::skybox_desc>(tag);
			BOOST_REQUIRE(handle.is_ready());
			BOOST_TEST(handle.get()->material.has_error());
			const auto items = diagnostics_of(handle);
			BOOST_TEST((at(items, res::diagnostic_kind::reference, "/material") != nullptr));
		}
	}
	BOOST_TEST(reads() == 0u);
}

BOOST_AUTO_TEST_CASE(AParent_IsNotLoaded_TheDescFailsAsWithAMissingParent)
{
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const res::tag tag = put(std::format("child_{}.desc", protocol),
				std::format(R"json({{"__type":"material_desc","__parent":"{}://base.desc"}})json", protocol));
			const auto handle = require_loaded<desc::desc_base>(tag);
			BOOST_TEST(handle.has_error());
			const auto items = diagnostics_of(handle);
			BOOST_TEST((at(items, res::diagnostic_kind::reference, "/__parent") != nullptr));
		}
	}
	BOOST_TEST(reads() == 0u);
}

BOOST_AUTO_TEST_CASE(AWorldParent_IsNotLoaded_TheWorldFailsAsWithAMissingParent)
{
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const res::tag tag = put(std::format("world_{}.desc", protocol),
				std::format(R"json({{"__type":"world_desc","__parent":"{}://w.desc","name":"derived"}})json", protocol));
			const auto handle = require_loaded<scn::world_desc>(tag);
			BOOST_TEST(handle.has_error());
			const auto items = diagnostics_of(handle);
			BOOST_TEST((at(items, res::diagnostic_kind::reference, "/__parent") != nullptr));
		}
	}
	BOOST_TEST(reads() == 0u);
}

BOOST_AUTO_TEST_CASE(ASampler_IsEmptied_TheOtherSlotsStay)
{
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const res::tag tag = put(std::format("samplers_{}.desc", protocol),
				std::format(R"json({{"__type":"material_desc","samplers":["res://a.png","{}://b.png",null]}})json", protocol));
			const auto handle = require_loaded<scn::material_desc>(tag);
			BOOST_REQUIRE(handle.is_ready());
			const auto& samplers = handle.get()->samplers_textures;
			BOOST_REQUIRE_EQUAL(samplers.size(), 3u);
			BOOST_TEST(samplers[0].string() == "res://a.png");
			BOOST_TEST(!samplers[1].is_valid());
			BOOST_TEST(!samplers[2].is_valid());
			BOOST_TEST((at(diagnostics_of(handle), res::diagnostic_kind::reference, "/samplers/1") != nullptr));
		}
	}
	BOOST_TEST(reads() == 0u);
}

// The assembler reads a level's overrides through a copy without a tag: the owner is unknown, the stricter rule holds.
BOOST_AUTO_TEST_CASE(AnOverrideReadByTheAssembler_LoadsNothing)
{
	const res::tag sky = put("sky.desc", R"json({"__type":"skybox_desc","material":"memory://t/sky_material.desc"})json");
	put("sky_material.desc", R"json({"__type":"material_desc","queue":"transparent"})json");
	const auto parent = require_loaded<desc::desc_base>(sky);
	BOOST_REQUIRE(parent.is_ready());

	entt::registry reg;
	for (const std::string_view protocol : k_closed) {
		BOOST_TEST_CONTEXT(protocol) {
			const auto e = reg.create();
			const json::object overrides = json::parse(std::format(R"json({{"material":"{}://m.desc"}})json", protocol)).as_object();
			assembler->assemble_and_apply(reg, e, "skybox_desc", parent, overrides, "sky");
			pump();
			const auto* component = reg.try_get<scn::material_desc_component>(e);
			BOOST_REQUIRE(component);
			BOOST_TEST(component->mlt_desc.has_error());
		}
	}
	BOOST_TEST(reads() == 0u);
}

BOOST_AUTO_TEST_CASE(ADescOfAProtocol_MayReferToItsOwnProtocol)
{
	auto& user = *mounts.at("user");
	user.store(res::tag{ "user://child.desc" }, test_util::to_bytes(R"json({"__type":"material_desc","__parent":"user://base.desc"})json"));
	user.store(res::tag{ "user://base.desc" }, test_util::to_bytes(R"json({"__type":"material_desc","queue":"transparent"})json"));
	const auto handle = require_loaded<scn::material_desc>(res::tag{ "user://child.desc" });
	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST((handle.get()->queue == scn::pass_queue::TRANSPARENT));
}

BOOST_AUTO_TEST_CASE(ResEngineAndMemoryReferences_StayAsTheyWere)
{
	const res::tag tag = put("open.desc", R"json({"__type":"material_desc","shader_fragment":"res://shaders/a.frag",
		"shader_vertex":"engine://shaders/a.vert","samplers":["memory://t/a.png","res://b.png"]})json");
	const auto handle = require_loaded<scn::material_desc>(tag);
	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get()->fragment_shader.string() == "res://shaders/a.frag");
	BOOST_TEST(handle.get()->vertex_shader.string() == "engine://shaders/a.vert");
	BOOST_TEST(handle.get()->samplers_textures.size() == 2u);
	BOOST_TEST((at(diagnostics_of(handle), res::diagnostic_kind::reference, "/samplers/0") == nullptr));
}

BOOST_AUTO_TEST_SUITE_END()
