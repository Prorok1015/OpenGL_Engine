#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include <snk/resource/res_system.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/desc/desc_system.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

using namespace core::res;

namespace {

std::vector<std::byte> bytes_of(const std::string& str)
{
	std::vector<std::byte> result(str.size());
	std::memcpy(result.data(), str.data(), str.size());
	return result;
}

struct hardening_test_desc : desc::desc_base
{
	int value = 0;

	void copy_to(desc::desc_base& other) const override
	{
		if (auto* o = dynamic_cast<hardening_test_desc*>(&other)) {
			o->value = value;
		}
	}
	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (obj.contains("value")) value = static_cast<int>(obj.at("value").as_int64());
	}
	void serialize(boost::json::object& obj) const override { obj["value"] = value; }
};

struct desc_fixture
{
	desc_fixture()
	{
		std::filesystem::create_directories("./res");
		std::filesystem::create_directories("./sandbox");
		res_sys = std::make_unique<res::resource_system>(tasks);
		desc_sys = std::make_unique<desc::desc_system>(*res_sys);
		desc_sys->register_desc<hardening_test_desc>("hardening_test_desc");
		res_sys->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*res_sys), std::ref(*desc_sys));
	}

	res::tag put(const std::string& name, const std::string& json_text)
	{
		auto tag = res::tag{ "memory://hardening/" + name + ".desc" };
		BOOST_REQUIRE(res_sys->store(tag, bytes_of(json_text)));
		return tag;
	}

	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> res_sys;
	std::unique_ptr<desc::desc_system> desc_sys;
};

}

BOOST_AUTO_TEST_SUITE(ResHardeningTests)

BOOST_AUTO_TEST_CASE(Tag_DifferentStringsAreNotEqual)
{
	BOOST_CHECK(res::tag::make("a/b.txt") == res::tag::make("a/b.txt"));
	BOOST_CHECK(!(res::tag::make("a/b.txt") == res::tag::make("a/c.txt")));
	BOOST_CHECK(!(res::tag::make("a/b.txt") == res::tag{ "memory://a/b.txt" }));
}

BOOST_AUTO_TEST_CASE(Fnv1a64_GoldenValues)
{
	BOOST_CHECK_EQUAL(desc::desc_system::fnv1a_64(""), 0xcbf29ce484222325ull);
	BOOST_CHECK_EQUAL(desc::desc_system::fnv1a_64("a"), 0xaf63dc4c8601ec8cull);
	BOOST_CHECK_EQUAL(desc::desc_system::fnv1a_64("foobar"), 0x85944171f73967e8ull);
}

BOOST_AUTO_TEST_CASE(ControlBlock_ThenRacingWithCompletion_FiresExactlyOnce)
{
	constexpr int iterations = 2000;
	for (int i = 0; i < iterations; ++i) {
		res_control_block<int> block;
		std::atomic<int> fired = 0;
		std::atomic<int> wrong_value = 0;
		std::atomic<bool> go = false;

		std::thread completer([&] {
			while (!go) std::this_thread::yield();
			block.set_ready(7);
		});
		std::thread subscriber([&] {
			while (!go) std::this_thread::yield();
			// Boost.Test is not thread-safe: record results here, check after join.
			block.on_settled([&](auto& b) {
				if (b.data != 7) ++wrong_value;
				++fired;
			});
		});
		go = true;
		completer.join();
		subscriber.join();
		BOOST_REQUIRE_EQUAL(fired.load(), 1);
		BOOST_REQUIRE_EQUAL(wrong_value.load(), 0);
	}
}

BOOST_AUTO_TEST_CASE(ControlBlock_ThrowingCallbackKeepsBlockReadyAndOthersFire)
{
	res_control_block<int> block;
	int before = 0, after = 0;
	block.on_settled([&](auto&) { ++before; });
	block.on_settled([&](auto&) { throw std::runtime_error("callback failure"); });
	block.on_settled([&](auto&) { ++after; });

	BOOST_CHECK_NO_THROW(block.set_ready(3));
	BOOST_CHECK(block.is_ready());
	BOOST_CHECK(!block.has_error());
	BOOST_CHECK_EQUAL(block.data, 3);
	BOOST_CHECK_EQUAL(before, 1);
	BOOST_CHECK_EQUAL(after, 1);

	// A throwing callback registered after completion must not propagate either.
	BOOST_CHECK_NO_THROW(block.on_settled([](auto&) { throw std::runtime_error("late failure"); }));
}

BOOST_AUTO_TEST_CASE(ControlBlock_SecondCompletionIsIgnored)
{
	res_control_block<int> block;
	block.set_ready(1);
	block.set_error("late error");
	block.set_ready(2);
	BOOST_CHECK(block.is_ready());
	BOOST_CHECK(!block.has_error());
	BOOST_CHECK_EQUAL(block.data, 1);
	BOOST_CHECK(block.error_msg.empty());

	res_control_block<int> failed;
	failed.set_error("first");
	failed.set_ready(5);
	BOOST_CHECK(failed.has_error());
	BOOST_CHECK_EQUAL(failed.error_msg, "first");
}

BOOST_AUTO_TEST_CASE(RequireSync_ThrowingResolver_EndsInErrorStatus)
{
	struct throwing_resolver : core::res::res_resolver_interface {
		using res_resolver_interface::res_resolver_interface;
		async_raw_data resolve(const res::tag&) const override { throw std::runtime_error("resolver exploded"); }
	};

	desc_fixture f;
	f.res_sys->registrate_resolver<throwing_resolver>("throwres");
	const res::tag tag{ "throwres://x/y.txt" };

	auto handle = f.res_sys->require_sync<res::text_resource>(tag);
	BOOST_CHECK(handle.has_error());
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.get_control_block()->error_msg.find("resolver exploded") != std::string::npos);

	// A second requester shares the failed block instead of waiting forever.
	auto again = f.res_sys->require_sync<res::text_resource>(tag);
	BOOST_CHECK(again.has_error());
}

BOOST_AUTO_TEST_CASE(ControlBlock_CallbackMayReenterThen)
{
	res_control_block<int> block;
	int inner = 0;
	block.on_settled([&](auto& b) { b.on_settled([&](auto&) { ++inner; }); });
	block.set_ready(1);
	BOOST_CHECK_EQUAL(inner, 1);
}

BOOST_AUTO_TEST_CASE(ControlBlock_WaitReturnsOnErrorWithoutAsserting)
{
	res_control_block<int> block;
	std::thread t([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		block.set_error("boom");
	});
	block.wait();
	t.join();
	BOOST_CHECK(block.has_error());
	BOOST_CHECK_EQUAL(block.error_msg, "boom");
}

BOOST_AUTO_TEST_CASE(RequireSync_MissingFile_ReturnsErrorWithoutCrash)
{
	desc_fixture f;
	auto handle = f.res_sys->require_sync<res::text_resource>(res::tag::make("definitely/missing_file.txt"));
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

// A format nobody registered an adapter for (a .gltf in a build without assimp) is a load error, not a crash.
BOOST_AUTO_TEST_CASE(RequireSync_NoAdapterForExtension_ReturnsErrorWithoutCrash)
{
	desc_fixture f;
	const auto tag = res::tag{ "memory://hardening/model.gltf" };
	BOOST_REQUIRE(f.res_sys->store(tag, bytes_of("{}")));
	auto handle = f.res_sys->require_sync<res::text_resource>(tag);
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

BOOST_AUTO_TEST_CASE(RequireSync_UnknownProtocol_FailsLoad)
{
	desc_fixture f;
	auto handle = f.res_sys->require_sync<res::text_resource>(res::tag{ "nosuchproto://x/y.txt" });
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

BOOST_AUTO_TEST_CASE(Desc_Valid_LoadsAndInheritsParent)
{
	desc_fixture f;
	auto parent = f.put("ok_parent", R"({"__type":"hardening_test_desc","value":5})");
	auto child = f.put("ok_child", R"({"__type":"hardening_test_desc","__parent":"memory://hardening/ok_parent.desc"})");
	auto handle = f.res_sys->require_sync<desc::desc_base>(child);
	auto loaded = std::dynamic_pointer_cast<hardening_test_desc>(handle.get_sync());
	BOOST_REQUIRE(loaded);
	BOOST_CHECK_EQUAL(loaded->value, 5);
}

BOOST_AUTO_TEST_CASE(Desc_UnknownType_IsLoadError)
{
	desc_fixture f;
	auto tag = f.put("unknown_type", R"({"__type":"no_such_type"})");
	auto handle = f.res_sys->require_sync<desc::desc_base>(tag);
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

BOOST_AUTO_TEST_CASE(Desc_MissingType_IsLoadError)
{
	desc_fixture f;
	auto tag = f.put("missing_type", R"({"value":1})");
	auto handle = f.res_sys->require_sync<desc::desc_base>(tag);
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

BOOST_AUTO_TEST_CASE(Desc_MissingParent_IsLoadError)
{
	desc_fixture f;
	auto tag = f.put("missing_parent", R"({"__type":"hardening_test_desc","__parent":"memory://hardening/nonexistent.desc"})");
	auto handle = f.res_sys->require_sync<desc::desc_base>(tag);
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

BOOST_AUTO_TEST_CASE(Desc_ParentCycle_IsLoadError)
{
	desc_fixture f;
	f.put("cycle_a", R"({"__type":"hardening_test_desc","__parent":"memory://hardening/cycle_b.desc"})");
	f.put("cycle_b", R"({"__type":"hardening_test_desc","__parent":"memory://hardening/cycle_a.desc"})");
	auto handle = f.res_sys->require_sync<desc::desc_base>(res::tag{ "memory://hardening/cycle_a.desc" });
	BOOST_CHECK(handle.get_sync() == nullptr);
	BOOST_CHECK(handle.has_error());
}

BOOST_AUTO_TEST_SUITE_END()
