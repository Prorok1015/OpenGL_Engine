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
	return module_entry{ &snk_module_api_version, &snk_module_build_info, &snk_create_module, &snk_destroy_module };
}

std::uint32_t wrong_major_version() { return SNK_MODULE_API_VERSION + 0x0001'0000u; }
std::uint32_t newer_minor_version() { return SNK_MODULE_API_VERSION + 1u; }

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

BOOST_AUTO_TEST_CASE(IncompleteEntryIsRefused)
{
	auto entry = generated_entry();
	entry.destroy = nullptr;
	BOOST_CHECK_THROW(loader.load(entry, storage), std::logic_error);
	BOOST_CHECK_THROW(loader.load(module_entry{}, storage), std::logic_error);
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_SUITE_END()
