#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// A node is its label, then the value of each dependency in brackets: "l(b(x))". Raw data: the label on the first
// line, one dependency tag per line after it.
std::vector<std::string> lines_of(const std::vector<std::byte>& raw)
{
	std::vector<std::string> lines;
	std::istringstream stream(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
	for (std::string line; std::getline(stream, line);) {
		if (!line.empty()) lines.push_back(line);
	}
	return lines;
}

struct base_node : res::text_resource {
	base_node(const res::tag& tag, std::string text) : res::text_resource(tag, {}) { content = std::move(text); }
};

// Only a layer has an adapter for it.
struct layer_node : res::text_resource {
	layer_node(const res::tag& tag, std::string text) : res::text_resource(tag, {}) { content = std::move(text); }
};

template<class NODE>
std::shared_ptr<res::resource_entry> build_node(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps)
{
	const auto lines = lines_of(raw);
	std::string text = lines.empty() ? std::string{} : lines.front();
	if (!deps.empty()) {
		text += "(";
		for (std::size_t i = 0; i < deps.size(); ++i) {
			if (i) text += ",";
			text += deps.get<base_node>(deps.entries()[i].dep.target)->str();
		}
		text += ")";
	}
	return std::make_shared<NODE>(tag, std::move(text));
}

std::vector<res::res_dep> node_dependencies(const std::vector<std::byte>& raw)
{
	std::vector<res::res_dep> deps;
	const auto lines = lines_of(raw);
	for (std::size_t i = 1; i < lines.size(); ++i) {
		deps.push_back(res::make_dep<base_node>(res::tag{ lines[i] }));
	}
	return deps;
}

struct base_node_adapter : core::res::adapter_interface {
	static inline auto INFO = res::adapter_info::make<base_node>({ "node" });
	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>& raw) const override { return node_dependencies(raw); }
	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps) const override
	{
		return build_node<base_node>(tag, raw, deps);
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

// Counts its instances: a module's adapter must be gone with the layer, whatever the root still loads.
struct layer_node_adapter : core::res::adapter_interface {
	static inline auto INFO = res::adapter_info::make<layer_node>({ "lnode" });
	static inline std::atomic<int> alive{ 0 };
	layer_node_adapter() { ++alive; }
	~layer_node_adapter() override { --alive; }
	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>& raw) const override { return node_dependencies(raw); }
	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps) const override
	{
		return build_node<layer_node>(tag, raw, deps);
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

// One adapter per type level, to see which one built the resource.
struct item_base : res::resource_entry {
	std::string made_by;
};
struct item_derived : item_base {
	using base_type = item_base;
};
struct item_adapter : core::res::adapter_interface {
	explicit item_adapter(std::string who) : who(std::move(who)) {}
	std::shared_ptr<res::resource_entry> deserialize(const res::tag&, const std::vector<std::byte>&) const override
	{
		auto item = std::make_shared<item_derived>();
		item->made_by = who;
		return item;
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
	std::string who;
};

// Holds the data of every request while `gate` is set, until release(): a load whose raw data arrives late.
class gated_resolver : public test_util::ram_resolver
{
public:
	explicit gated_resolver(const std::string_view prt) : ram_resolver(prt) {}

	async_raw_data resolve(const res::tag& tag) const override
	{
		if (!gate) {
			return ram_resolver::resolve(tag);
		}
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		std::lock_guard lock(mutex);
		held.emplace_back(block, files.at(tag));
		return block;
	}

	void release()
	{
		decltype(held) taken;
		{
			std::lock_guard lock(mutex);
			taken.swap(held);
		}
		for (auto& [block, data] : taken) {
			block->set_ready(std::move(data));
		}
	}

	std::atomic<bool> gate{ false };
	mutable std::mutex mutex;
	mutable std::vector<std::pair<std::shared_ptr<core::res::res_control_block<std::vector<std::byte>>>, std::vector<std::byte>>> held;
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

// The base's empty game layer has no limit: its writes go to the root's resolvers.
BOOST_AUTO_TEST_CASE(a_layer_without_a_write_limit_stores_and_erases_in_the_root)
{
	const auto tag = tag_of("stored.txt");
	BOOST_TEST(layer->store(tag, test_util::to_bytes("x")));
	BOOST_TEST(rs->exists(tag));
	BOOST_TEST(layer->exists(tag));
	BOOST_TEST(layer->erase(tag));
	BOOST_TEST(!rs->exists(tag));
	BOOST_TEST((layer->get_resource_layers() == rs->get_resource_layers()));
}

// A game module's layer writes only under memory://modules/<name>/ (#405, MODULE_LAYER.md): the base stays as it was,
// and reads still go through to it.
BOOST_AUTO_TEST_CASE(a_module_layer_writes_only_under_its_prefix)
{
	layer->limit_writes_to("memory://modules/game/");
	const res::tag own{ "memory://modules/game/stored.txt" };
	BOOST_TEST(layer->store(own, test_util::to_bytes("x")));
	BOOST_TEST(rs->exists(own));
	BOOST_TEST(layer->erase(own));
	BOOST_TEST(!rs->exists(own));

	const auto base = tag_of("base.txt");
	BOOST_REQUIRE(rs->store(base, test_util::to_bytes("base")));
	BOOST_TEST(layer->exists(base));
	BOOST_CHECK_THROW(layer->store(base, test_util::to_bytes("changed")), std::logic_error);
	BOOST_CHECK_THROW(layer->erase(base), std::logic_error);
	BOOST_TEST(rs->exists(base));
	BOOST_TEST(load<res::text_resource>(*layer, base).get()->str() == "base");

	for (const char* refused : { "res://stored.txt", "memory://modules/other/stored.txt", "memory://modules/game/../other/x",
			"memory://modules/game/", "memory://modules/gamex/stored.txt" }) {
		BOOST_TEST_CONTEXT(refused) {
			BOOST_CHECK_THROW(layer->store(res::tag{ refused }, test_util::to_bytes("x")), std::logic_error);
			BOOST_CHECK_THROW(layer->erase(res::tag{ refused }), std::logic_error);
		}
	}
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

BOOST_AUTO_TEST_CASE(the_most_specific_adapter_wins_across_the_chain)
{
	rs->registrate_adapter<item_adapter>(res::adapter_info::make<item_derived>({ "item" }), std::string{ "base" });
	layer->registrate_adapter<item_adapter>(res::adapter_info::make<item_base>({ "item" }), std::string{ "layer" });
	const auto tag = put("a.item", "x");

	// The base's adapter for the derived type beats the layer's for its base type: the parent's block.
	const auto derived = load<item_derived>(*layer, tag);
	BOOST_REQUIRE(derived.is_ready());
	BOOST_TEST(derived.get()->made_by == "base");
	BOOST_TEST((derived.get_control_block() == load<item_derived>(*rs, tag).get_control_block()));

	// For the base type the layer's own adapter is the most specific one.
	const auto base = load<item_base>(*layer, tag);
	BOOST_REQUIRE(base.is_ready());
	BOOST_TEST(base.get()->made_by == "layer");
}

BOOST_AUTO_TEST_CASE(a_change_below_a_base_resource_reaches_the_layer_resource_built_on_it)
{
	rs->registrate_adapter<base_node_adapter>(base_node_adapter::INFO);
	layer->registrate_adapter<layer_node_adapter>(layer_node_adapter::INFO);
	const auto x = put("x.node", "x");
	const auto b = put("b.node", "b\n" + x.string());
	const auto l = put("l.lnode", "l\n" + b.string());

	const auto first = load<layer_node>(*layer, l);
	BOOST_REQUIRE(first.is_ready());
	BOOST_TEST(first.get()->str() == "l(b(x))");
	int notified = 0;
	layer->watch(l, this, [&notified](const res::tag&) { ++notified; });

	rs->store(x, test_util::to_bytes("x2"));
	rs->signal_changed(x); // the base evicts b; the layer must evict l, which is built on b
	pump();
	BOOST_TEST(notified == 1);
	const auto second = load<layer_node>(*layer, l);
	BOOST_REQUIRE(second.is_ready());
	BOOST_TEST(second.get()->str() == "l(b(x2))");
	layer->unwatch(l, this);
}

BOOST_AUTO_TEST_CASE(destroying_the_layer_releases_what_it_kept_in_the_parent)
{
	rs->registrate_adapter<base_node_adapter>(base_node_adapter::INFO);
	layer->registrate_adapter<layer_node_adapter>(layer_node_adapter::INFO);
	const auto x = put("x.node", "x");
	const auto b = put("b.node", "b\n" + x.string());
	const auto l = put("l.lnode", "l\n" + b.string());

	BOOST_REQUIRE(load<layer_node>(*layer, l).is_ready());
	BOOST_TEST(rs->layer_needs_count() > 0u);
	layer.reset();
	BOOST_TEST(rs->layer_needs_count() == 0u);
}

BOOST_AUTO_TEST_CASE(destroying_the_layer_drops_its_adapters_while_the_root_still_loads)
{
	rs->unregistrate_resolver(res::tag::memory);
	auto& gated = rs->registrate_resolver<gated_resolver>(res::tag::memory);
	rs->registrate_adapter<base_node_adapter>(base_node_adapter::INFO);
	layer->registrate_adapter<layer_node_adapter>(layer_node_adapter::INFO);
	BOOST_REQUIRE(layer_node_adapter::alive == 1);
	const auto x = put("x.node", "x");
	const auto l = put("l.lnode", "l\n" + x.string());

	const auto pending = layer->require<layer_node>(l); // its raw data is there; x will not be
	gated.gate = true;
	pump(); // l waits for the parent's block of x
	BOOST_REQUIRE(!pending.is_ready());

	layer.reset();
	BOOST_TEST(layer_node_adapter::alive == 0); // nothing on the root's side kept it
	BOOST_TEST(pending.has_error());

	gated.gate = false;
	gated.release(); // the parent's block settles after the layer is gone
	pump();
	BOOST_TEST(pending.has_error());
}

BOOST_AUTO_TEST_CASE(erasing_through_a_layer_leaves_the_base_pins)
{
	const auto tag = put("o.txt", "o");
	rs->warmup<res::text_resource>(tag);
	layer->warmup<res::text_resource>(tag);
	pump();
	BOOST_REQUIRE(rs->pinned_count() == 1u);
	BOOST_REQUIRE(layer->pinned_count() == 1u);

	BOOST_TEST(layer->erase(tag));
	BOOST_TEST(!rs->exists(tag)); // the data is the root's resolvers': gone for everyone
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(layer->pinned_count() == 0u);
}

BOOST_AUTO_TEST_CASE(a_layer_leaves_the_base_cache_as_it_was)
{
	const auto a = put("a.txt", "a");
	const auto held = load<res::text_resource>(*rs, a);
	const std::size_t before = rs->live_resources();

	layer->registrate_adapter<res::text_adapter>(res::text_adapter::INFO);
	{
		const auto own = load<res::text_resource>(*layer, put("c.txt", "c"));
		BOOST_REQUIRE(own.is_ready());
		BOOST_TEST(rs->live_resources() == before); // the layer's own resources are not in the base cache
	}
	layer.reset();
	BOOST_TEST(rs->live_resources() == before);
	BOOST_TEST((load<res::text_resource>(*rs, a).get_control_block() == held.get_control_block()));
}

BOOST_AUTO_TEST_CASE(a_warmup_in_a_layer_serves_the_current_block)
{
	const auto tag = put("w.txt", "old");
	layer->warmup<res::text_resource>(tag);
	pump();
	const auto first = load<res::text_resource>(*layer, tag);
	BOOST_REQUIRE(first.is_ready());

	rs->store(tag, test_util::to_bytes("new"));
	rs->signal_changed(tag);
	pump();
	const auto second = load<res::text_resource>(*layer, tag);
	BOOST_REQUIRE(second.is_ready());
	BOOST_TEST(second.get()->str() == "new"); // not the pinned old block
}

// Real threads (TSan): the layer goes while its loads wait for the root's blocks, which settle afterwards.
BOOST_AUTO_TEST_CASE(destroying_a_layer_during_threaded_loads)
{
	ds::task_system threaded;
	res::resource_system root{ threaded, res::mount_table{} };
	auto& gated = root.registrate_resolver<gated_resolver>(res::tag::memory);
	root.registrate_adapter<base_node_adapter>(base_node_adapter::INFO);
	const res::tag x{ "memory://threaded/x.node" };
	BOOST_REQUIRE(root.store(x, test_util::to_bytes("x")));
	// Stored before any load: the pool threads resolve the dependencies while the test goes on.
	constexpr int rounds = 20;
	std::vector<res::tag> tops;
	for (int round = 0; round < rounds; ++round) {
		tops.emplace_back("memory://threaded/l" + std::to_string(round) + ".lnode");
		BOOST_REQUIRE(root.store(tops.back(), test_util::to_bytes("l\n" + x.string())));
	}

	for (int round = 0; round < rounds; ++round) {
		BOOST_TEST_CONTEXT("round " << round) {
			const res::tag& l = tops[static_cast<std::size_t>(round)];
			auto in_layer = std::make_unique<res::resource_system>(root);
			in_layer->registrate_adapter<layer_node_adapter>(layer_node_adapter::INFO);
			gated.gate = (round % 2) == 0; // half the rounds hold the raw data back until the layer is gone
			const auto pending = in_layer->require<layer_node>(l);
			std::this_thread::sleep_for(std::chrono::microseconds(200 * (round % 5)));
			in_layer.reset();
			BOOST_TEST(layer_node_adapter::alive == 0);
			gated.gate = false;
			gated.release();
			threaded.drain_main(std::chrono::milliseconds(50));
			root.signal_changed(x); // a fresh x for the next round
		}
	}
	threaded.drain_main(std::chrono::milliseconds(50));
}

BOOST_AUTO_TEST_SUITE_END()
