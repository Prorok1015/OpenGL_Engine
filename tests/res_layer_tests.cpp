#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <boost/json.hpp>
#include <functional>
#include <optional>
#include <string>

// A resource layer (#220 PR-4, MODULE_LAYER.md): a resource_system over a parent that shares its resolvers and
// signals, and keeps in its own cache only what its own adapters load.

namespace {

struct layer_fixture : test_util::ram_res_env {
	layer_fixture() { layer.emplace(*rs); }
	~layer_fixture() { layer.reset(); }

	template<class T>
	res::res_handle<T> load(res::resource_system& system, const res::tag& tag)
	{
		auto handle = system.require<T>(tag);
		pump();
		return handle;
	}

	std::optional<res::resource_system> layer;
};

struct game_desc : desc::desc_base {
	int value = 0;

	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (const auto* v = obj.if_contains("value")) value = static_cast<int>(v->to_number<std::int64_t>());
	}
	void serialize(boost::json::object& obj) const override { obj["value"] = value; }
};

}

BOOST_FIXTURE_TEST_SUITE(res_layer_tests, layer_fixture)

BOOST_AUTO_TEST_CASE(a_type_without_a_layer_adapter_is_the_parents_block)
{
	const auto tag = put("a.txt", "hello");
	const auto base = load<res::text_resource>(*rs, tag);
	const auto through_layer = load<res::text_resource>(*layer, tag);
	BOOST_TEST((through_layer.get_control_block() == base.get_control_block()));
	BOOST_REQUIRE(through_layer.is_ready());
	BOOST_TEST(through_layer.get()->str() == "hello");
	BOOST_TEST(layer->live_resources() == 0u);
	BOOST_TEST(layer->parent() == &*rs);
}

BOOST_AUTO_TEST_CASE(the_layers_own_adapter_loads_into_the_layer)
{
	layer->registrate_adapter<res::text_adapter>(res::text_adapter::INFO);
	const auto tag = put("a.txt", "hello");
	const auto base = load<res::text_resource>(*rs, tag);
	auto own = load<res::text_resource>(*layer, tag);
	BOOST_TEST((own.get_control_block() != base.get_control_block()));
	BOOST_REQUIRE(own.is_ready());
	BOOST_TEST(own.get()->str() == "hello");
	BOOST_TEST(layer->live_resources() == 1u);
	own = {};
	BOOST_TEST(layer->live_resources() == 0u);
}

BOOST_AUTO_TEST_CASE(a_change_signalled_on_the_root_reaches_the_layer)
{
	layer->registrate_adapter<res::text_adapter>(res::text_adapter::INFO);
	const auto tag = put("a.txt", "old");
	const auto first = load<res::text_resource>(*layer, tag);
	int notified = 0;
	layer->watch(tag, this, [&notified](const res::tag&) { ++notified; });

	rs->store(tag, test_util::to_bytes("new"));
	rs->signal_changed(tag);
	pump();
	BOOST_TEST(notified == 1);
	const auto second = load<res::text_resource>(*layer, tag);
	BOOST_TEST((second.get_control_block() != first.get_control_block()));
	BOOST_REQUIRE(second.is_ready());
	BOOST_TEST(second.get()->str() == "new");
	layer->unwatch(tag, this);
}

BOOST_AUTO_TEST_CASE(a_change_signalled_on_the_layer_reaches_the_root)
{
	const auto tag = put("a.txt", "old");
	const auto first = load<res::text_resource>(*rs, tag);
	rs->store(tag, test_util::to_bytes("new"));
	layer->signal_changed(tag);
	pump();
	const auto second = load<res::text_resource>(*rs, tag);
	BOOST_TEST((second.get_control_block() != first.get_control_block()));
	BOOST_REQUIRE(second.is_ready());
	BOOST_TEST(second.get()->str() == "new");
}

BOOST_AUTO_TEST_CASE(destroying_the_layer_cancels_its_pending_loads)
{
	layer->registrate_adapter<res::text_adapter>(res::text_adapter::INFO);
	const auto tag = put("a.txt", "hello");
	const auto pending = layer->require<res::text_resource>(tag); // not pumped: its load task is still queued
	BOOST_TEST(!pending.is_ready());
	layer.reset();
	pump();
	BOOST_TEST(pending.has_error());
}

BOOST_AUTO_TEST_CASE(store_exists_and_erase_go_to_the_root)
{
	const auto tag = tag_of("stored.txt");
	BOOST_TEST(layer->store(tag, test_util::to_bytes("x")));
	BOOST_TEST(rs->exists(tag));
	BOOST_TEST(layer->exists(tag));
	BOOST_TEST(layer->erase(tag));
	BOOST_TEST(!rs->exists(tag));
	BOOST_TEST((layer->get_resource_layers() == rs->get_resource_layers()));
}

BOOST_AUTO_TEST_CASE(a_desc_type_of_the_layer_loads_only_through_the_layer)
{
	desc::desc_system base_descs{ *rs };
	rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs), std::ref(base_descs));
	{
		desc::desc_system layer_descs{ *layer };
		layer_descs.register_desc<game_desc>("game_desc");
		layer->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*layer), std::ref(layer_descs));

		const auto tag = put("g.desc", R"({ "__type": "game_desc", "value": 3 })");
		{
			const auto through_layer = load<desc::desc_base>(*layer, tag);
			BOOST_REQUIRE(through_layer.is_ready());
			const auto* game = dynamic_cast<const game_desc*>(through_layer.get().get());
			BOOST_REQUIRE(game != nullptr);
			BOOST_TEST(game->value == 3);
			BOOST_TEST(layer->live_resources() == 1u);
		}
		BOOST_TEST(load<desc::desc_base>(*rs, tag).has_error());
		layer.reset(); // the layer's adapter uses layer_descs: the layer goes first
	}
	rs->unregistrate_adapter(core::desc::desc_adapter_t::INFO);
}

BOOST_AUTO_TEST_SUITE_END()
