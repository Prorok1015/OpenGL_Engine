#include <boost/test/unit_test.hpp>
#include "app_module_loader.hpp"

#include <stdexcept>
#include <string>

namespace {

	struct fake_module final : app::modules::module_interface {
		fake_module(std::string name_, std::vector<std::string>& log_, bool throw_on_init_ = false,
			std::vector<std::string_view> deps_ = {}, bool throw_on_shutdown_ = false)
			: m_name(std::move(name_)), log(log_), throw_on_init(throw_on_init_), deps(std::move(deps_)),
			throw_on_shutdown(throw_on_shutdown_) {}

		void register_services(ds::app_data_storage&) override { log.push_back("register " + m_name); }
		void initialize_services(ds::app_data_storage&) override
		{
			if (throw_on_init) {
				throw std::runtime_error("init failed: " + m_name);
			}
			log.push_back("init " + m_name);
		}
		void shutdown_services(ds::app_data_storage&) override
		{
			log.push_back("shutdown " + m_name);
			if (throw_on_shutdown) {
				throw std::runtime_error("shutdown failed: " + m_name);
			}
		}
		std::string_view name() const override { return m_name; }
		std::vector<std::string_view> depends_on() const override { return deps; }

		std::string m_name;
		std::vector<std::string>& log;
		bool throw_on_init;
		std::vector<std::string_view> deps;
		bool throw_on_shutdown;
	};

}

BOOST_AUTO_TEST_SUITE(ModuleLoaderTests)

BOOST_AUTO_TEST_CASE(ThrowingInitializeRollsBackInReverseAndRethrows)
{
	std::vector<std::string> log;
	ds::app_data_storage storage;
	app::modules::module_loader loader;
	loader.add_module(std::make_unique<fake_module>("a", log));
	loader.add_module(std::make_unique<fake_module>("b", log, false, std::vector<std::string_view>{ "a" }));
	loader.add_module(std::make_unique<fake_module>("c", log, true, std::vector<std::string_view>{ "b" }));
	loader.add_module(std::make_unique<fake_module>("d", log, false, std::vector<std::string_view>{ "c" }));

	loader.register_all_services(storage);
	log.clear();

	BOOST_CHECK_THROW(loader.initialize_all_services(storage), std::runtime_error);

	const std::vector<std::string> expected{ "init a", "init b", "shutdown b", "shutdown a" };
	BOOST_CHECK_EQUAL_COLLECTIONS(log.begin(), log.end(), expected.begin(), expected.end());

	// Rolled back: a later shutdown must not touch the modules again.
	log.clear();
	loader.shutdown_all_services(storage);
	BOOST_CHECK(log.empty());
}

BOOST_AUTO_TEST_CASE(RollbackContinuesWhenShutdownThrows)
{
	std::vector<std::string> log;
	ds::app_data_storage storage;
	app::modules::module_loader loader;
	loader.add_module(std::make_unique<fake_module>("a", log));
	loader.add_module(std::make_unique<fake_module>("b", log, false, std::vector<std::string_view>{ "a" }, true));
	loader.add_module(std::make_unique<fake_module>("c", log, true, std::vector<std::string_view>{ "b" }));

	BOOST_CHECK_THROW(loader.initialize_all_services(storage), std::runtime_error);

	const std::vector<std::string> expected{ "init a", "init b", "shutdown b", "shutdown a" };
	BOOST_CHECK_EQUAL_COLLECTIONS(log.begin(), log.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(SuccessfulLifecycleShutsDownInReverseOrder)
{
	std::vector<std::string> log;
	ds::app_data_storage storage;
	app::modules::module_loader loader;
	loader.add_module(std::make_unique<fake_module>("a", log));
	loader.add_module(std::make_unique<fake_module>("b", log, false, std::vector<std::string_view>{ "a" }));

	loader.register_all_services(storage);
	loader.initialize_all_services(storage);
	log.clear();
	loader.shutdown_all_services(storage);

	const std::vector<std::string> expected{ "shutdown b", "shutdown a" };
	BOOST_CHECK_EQUAL_COLLECTIONS(log.begin(), log.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_SUITE_END()
