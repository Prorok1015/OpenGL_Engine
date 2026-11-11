#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_loader.hpp>
#include <stdexcept>

// The C entry point of a game module, exercised without dlopen: the exported functions are
// declared in this binary and their addresses are put into a module_entry (EPIC-52 #21 will
// resolve the same four names from a real shared library).

namespace {

struct entry_service { int v = 7; };

int g_alive = 0;
int g_destroyed_through_entry = 0;

struct sample_module final : app::modules::module_interface
{
	sample_module() { ++g_alive; }
	~sample_module() override { --g_alive; }

	void register_services(ds::app_data_storage& data) override { data.construct<entry_service>(); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage& data) override { data.destruct<entry_service>(); }
	std::string_view name() const override { return "sample"; }
};


// The generated function snk_module_build_info() hides the struct of the same name below.
using build_info_t = snk_module_build_info;

}

// Generates snk_module_api_version / snk_module_build_info / snk_create_module / snk_destroy_module
// with their real names. These are unprefixed C symbols, so it may appear only once in this
// binary: every other test builds its module_entry from hand-written functions below.
SNK_DECLARE_MODULE(sample_module)

namespace {

using app::modules::module_entry;

module_entry generated_entry()
{
	return SNK_MODULE_ENTRY(snk_);
}

std::uint32_t wrong_major_version() { return SNK_MODULE_API_VERSION + 0x0001'0000u; }
// What a module built before the resource control block layout of #595 reports.
std::uint32_t api_1_0() { return 0x0001'0000u; }
std::uint32_t newer_minor_version() { return SNK_MODULE_API_VERSION + 1u; }
// A module built before the game actions of #604.
std::uint32_t api_3_1() { return 0x0003'0001u; }
// A module built before inp::fixed_action_state.
std::uint32_t api_3_2() { return 0x0003'0002u; }
// A module built before gs::module_context::observe.
std::uint32_t api_3_3() { return 0x0003'0003u; }
// A module built before inp::action_input.
std::uint32_t api_3_4() { return 0x0003'0004u; }
// A module built before snk_module_layout_key (#614).
std::uint32_t api_3_5() { return 0x0003'0005u; }
// A module built before gs::module_context::input_stack (#625).
std::uint32_t api_3_6() { return 0x0003'0006u; }
// A module built before scn::spawn_prefab.
std::uint32_t api_3_7() { return 0x0003'0007u; }
// A module built before resource_system::refused_block (#697).
std::uint32_t api_3_8() { return 0x0003'0008u; }
// A module built before scn::request_level.
std::uint32_t api_3_9() { return 0x0003'0009u; }
// A module built before gs::module_context::register_component (#606).
std::uint32_t api_3_11() { return 0x0003'000Bu; }
// A module built before scn::animation_finished.
std::uint32_t api_3_12() { return 0x0003'000Cu; }
// A module built before the bus journal (#605).
std::uint32_t api_3_13() { return 0x0003'000Du; }

const build_info_t* info_with(std::uint32_t api, std::uint32_t compiler, std::uint32_t flags)
{
	static build_info_t info;
	info = { api, compiler, flags };
	return &info;
}
const build_info_t* wrong_major_info() { return info_with(wrong_major_version(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); }
const build_info_t* wrong_debug_flag_info()
{
	const auto host = app::modules::make_build_info();
	return info_with(host.api, host.compiler, host.flags ^ app::modules::build_flag::debug);
}
const build_info_t* wrong_crt_flag_info()
{
	const auto host = app::modules::make_build_info();
	return info_with(host.api, host.compiler, host.flags ^ app::modules::build_flag::static_crt);
}
const build_info_t* wrong_compiler_info()
{
	const auto host = app::modules::make_build_info();
	return info_with(host.api, ((host.compiler >> 16) % 3 + 1) << 16, host.flags);
}
// The host's compiler family, one version (MSVC: _MSC_VER, GCC/Clang: major) older or newer.
const build_info_t* older_compiler_info()
{
	const auto host = app::modules::make_build_info();
	return info_with(host.api, host.compiler - 1, host.flags);
}
const build_info_t* newer_compiler_info()
{
	const auto host = app::modules::make_build_info();
	return info_with(host.api, host.compiler + 1, host.flags);
}

constexpr std::uint32_t compiler_of(std::uint32_t family, std::uint32_t version) { return (family << 16) | version; }

void counting_destroy(app::modules::module_interface* m)
{
	++g_destroyed_through_entry;
	delete m;
}

struct entry_fixture
{
	entry_fixture() { g_alive = 0; g_destroyed_through_entry = 0; }
	app::modules::module_loader loader;
	ds::app_data_storage storage;
};

}

BOOST_FIXTURE_TEST_SUITE(AppModuleEntryTests, entry_fixture)

BOOST_AUTO_TEST_CASE(GeneratedFunctionsReportApiVersionAndBuildInfoOfThisBinary)
{
	BOOST_TEST(snk_module_api_version() == SNK_MODULE_API_VERSION);
	const build_info_t* info = snk_module_build_info();
	BOOST_REQUIRE(info != nullptr);
	const auto host = app::modules::make_build_info();
	BOOST_TEST(info->api == host.api);
	BOOST_TEST(info->compiler == host.compiler);
	BOOST_TEST(info->flags == host.flags);
	BOOST_TEST(host.compiler != 0u);
}

BOOST_AUTO_TEST_CASE(AModuleBuiltAgainstApi1IsRefused)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	auto entry = generated_entry();
	entry.api_version = &api_1_0;
	BOOST_TEST(app::modules::check_module_entry(entry).find("api major version 1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LoadThroughEntryCreatesRunsAndUnloads)
{
	loader.load(generated_entry(), storage);
	BOOST_TEST(g_alive == 1);
	BOOST_TEST((loader.state("sample") == app::modules::module_state::initialized));
	BOOST_TEST(storage.has_value<entry_service>());

	loader.unload("sample", storage);
	BOOST_TEST(g_alive == 0); // destroyed through snk_destroy_module
	BOOST_TEST(storage.size() == 0u);
}

BOOST_AUTO_TEST_CASE(ModuleIsDestroyedByTheEntrysOwnDestroyFunction)
{
	auto entry = generated_entry();
	entry.destroy = &counting_destroy;
	loader.load(entry, storage);
	loader.unload("sample", storage);
	BOOST_TEST(g_destroyed_through_entry == 1);
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_CASE(LoaderDestructionAlsoDestroysThroughTheEntry)
{
	{
		app::modules::module_loader local;
		auto entry = generated_entry();
		entry.destroy = &counting_destroy;
		local.load(entry, storage);
		local.shutdown_all_services(storage);
	}
	BOOST_TEST(g_destroyed_through_entry == 1);
}

BOOST_AUTO_TEST_CASE(WrongApiMajorIsRefusedBeforeAnythingIsCreated)
{
	auto entry = generated_entry();
	entry.api_version = &wrong_major_version;
	BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);
	BOOST_TEST(storage.size() == 0u);
	BOOST_TEST(!loader.state("sample").has_value());
}

BOOST_AUTO_TEST_CASE(TheGameActionsAreMinor_AndAModuleBuiltFor3_1StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 2u);
	auto entry = generated_entry();
	entry.api_version = &api_3_1;
	entry.build_info = +[]() { return info_with(api_3_1(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(TheFixedActionsAreMinor3_3_AndAModuleBuiltFor3_2StillLoads)
{
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 3u);
	auto entry = generated_entry();
	entry.api_version = &api_3_2;
	entry.build_info = +[]() { return info_with(api_3_2(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(ComponentObserversAreMinor3_4_AndAModuleBuiltFor3_3StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 4u);
	auto entry = generated_entry();
	entry.api_version = &api_3_3;
	entry.build_info = +[]() { return info_with(api_3_3(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(ActionReachIsMinor3_5_AndAModuleBuiltFor3_4StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 5u);
	auto entry = generated_entry();
	entry.api_version = &api_3_4;
	entry.build_info = +[]() { return info_with(api_3_4(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

// #614: the layout key is an optional export; a module built for 3.5 has none and loads, checked by version only.
BOOST_AUTO_TEST_CASE(TheLayoutKeyIsMinor3_6_AndAModuleBuiltFor3_5WithoutItStillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 6u);
	auto entry = generated_entry();
	entry.api_version = &api_3_5;
	entry.build_info = +[]() { return info_with(api_3_5(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	entry.layout_key = nullptr;
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
	BOOST_TEST(app::modules::check_module_layout(entry, 0x1234u).empty());
}

// Without snk/game/gs_module_layout.h the generated export reports 0, which no host checks. This TU does not include it,
// and neither does the shared PCH (cmake/pch/snk_pch.h: standard and third-party headers only); if either did, the
// export would report the engine's key and this test would fail.
BOOST_AUTO_TEST_CASE(AModuleThatDoesNotKnowTheLayoutsReportsZeroAndIsNotChecked)
{
	const module_entry entry = generated_entry();
	BOOST_REQUIRE(entry.layout_key != nullptr);
	BOOST_TEST(entry.layout_key() == 0u);
	BOOST_TEST(app::modules::check_module_layout(entry, 0x1234u).empty());
}

BOOST_AUTO_TEST_CASE(TheInputStackFromCodeIsMinor3_7_AndAModuleBuiltFor3_6StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 7u);
	auto entry = generated_entry();
	entry.api_version = &api_3_6;
	entry.build_info = +[]() { return info_with(api_3_6(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(PrefabSpawnIsMinor3_8_AndAModuleBuiltFor3_7StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 8u);
	auto entry = generated_entry();
	entry.api_version = &api_3_7;
	entry.build_info = +[]() { return info_with(api_3_7(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(RefusedReferencesAreMinor3_9_AndAModuleBuiltFor3_8StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 9u);
	auto entry = generated_entry();
	entry.api_version = &api_3_8;
	entry.build_info = +[]() { return info_with(api_3_8(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(LevelSwitchIsMinor3_10_AndAModuleBuiltFor3_9StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 10u);
	auto entry = generated_entry();
	entry.api_version = &api_3_9;
	entry.build_info = +[]() { return info_with(api_3_9(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(SchemaComponentsOfAModuleAreMinor3_12_AndAModuleBuiltFor3_11StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 12u);
	auto entry = generated_entry();
	entry.api_version = &api_3_11;
	entry.build_info = +[]() { return info_with(api_3_11(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(AnimationFinishedIsMinor3_13_AndAModuleBuiltFor3_12StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 13u);
	auto entry = generated_entry();
	entry.api_version = &api_3_12;
	entry.build_info = +[]() { return info_with(api_3_12(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(TheBusJournalIsMinor3_14_AndAModuleBuiltFor3_13StillLoads)
{
	BOOST_TEST(app::modules::api_major(SNK_MODULE_API_VERSION) == 3u);
	BOOST_TEST(app::modules::api_minor(SNK_MODULE_API_VERSION) >= 14u);
	auto entry = generated_entry();
	entry.api_version = &api_3_13;
	entry.build_info = +[]() { return info_with(api_3_13(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_TEST(app::modules::check_module_entry(entry).empty());
}

BOOST_AUTO_TEST_CASE(ModuleNeedingNewerMinorIsRefused)
{
	auto entry = generated_entry();
	entry.api_version = &newer_minor_version;
	entry.build_info = +[]() { return info_with(newer_minor_version(), app::modules::make_build_info().compiler, app::modules::make_build_info().flags); };
	BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_CASE(BuildInfoApiDisagreeingWithApiVersionIsRefused)
{
	auto entry = generated_entry();
	entry.build_info = &wrong_major_info;
	BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_CASE(MismatchedBuildFlagsAreRefused)
{
	for (auto* info_fn : { &wrong_debug_flag_info, &wrong_crt_flag_info }) {
		auto entry = generated_entry();
		entry.build_info = info_fn;
		BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	}
	BOOST_TEST(g_alive == 0);
	BOOST_TEST(storage.size() == 0u);
}

BOOST_AUTO_TEST_CASE(MismatchedCompilerIsRefused)
{
	auto entry = generated_entry();
	entry.build_info = &wrong_compiler_info;
	BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_CASE(AnMsvcModuleNeedsTheEnginesMsvcOrANewerOne)
{
	using namespace app::modules;
	const std::uint32_t engine = compiler_of(compiler_id::msvc, 1944);
	const std::string older = compiler_problem(compiler_of(compiler_id::msvc, 1943), engine);
	BOOST_TEST(older.find("MSVC 1943") != std::string::npos);
	BOOST_TEST(older.find("MSVC 1944") != std::string::npos);
	BOOST_TEST(compiler_problem(engine, engine).empty());
	BOOST_TEST(compiler_problem(compiler_of(compiler_id::msvc, 1951), engine).empty());
}

BOOST_AUTO_TEST_CASE(GccAndClangModulesNeedTheEnginesMajor_AndAnotherFamilyIsRefused)
{
	using namespace app::modules;
	BOOST_TEST(!compiler_problem(compiler_of(compiler_id::gcc, 13), compiler_of(compiler_id::gcc, 14)).empty());
	BOOST_TEST(!compiler_problem(compiler_of(compiler_id::gcc, 15), compiler_of(compiler_id::gcc, 14)).empty());
	BOOST_TEST(compiler_problem(compiler_of(compiler_id::clang, 18), compiler_of(compiler_id::clang, 18)).empty());
	BOOST_TEST(!compiler_problem(compiler_of(compiler_id::msvc, 1951), compiler_of(compiler_id::clang, 18)).empty());
	BOOST_TEST(!compiler_problem(compiler_of(compiler_id::clang, 18), compiler_of(compiler_id::msvc, 1944)).empty());
}

BOOST_AUTO_TEST_CASE(AModuleOfAnOlderCompilerIsRefused_ANewerOneLoadsOnlyWithMsvc)
{
	auto older = generated_entry();
	older.build_info = &older_compiler_info;
	BOOST_CHECK_THROW(loader.load(older, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);

	auto newer = generated_entry();
	newer.build_info = &newer_compiler_info;
	if ((app::modules::make_build_info().compiler >> 16) == app::modules::compiler_id::msvc) {
		loader.load(newer, storage);
		BOOST_TEST(g_alive == 1);
		loader.unload("sample", storage);
	} else {
		BOOST_CHECK_THROW(loader.load(newer, storage), std::logic_error);
	}
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_CASE(IncompleteEntryIsRefused)
{
	auto entry = generated_entry();
	entry.destroy = nullptr;
	BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	BOOST_CHECK_THROW(loader.load(module_entry{}, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_SUITE_END()
