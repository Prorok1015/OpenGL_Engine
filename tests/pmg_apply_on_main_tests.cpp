#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/project/prj_json_text.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// apply_context::on_main (#761, EPIC-58 4.5): apply_migration on a worker, the engine's loads and saves of a desc on the
// main thread.
namespace {

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::map<std::string, std::string> content_of(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		const std::string relative = fs::relative(entry.path(), root).generic_string();
		if (relative.starts_with(".snk/")) continue;
		files[relative] = read_text(entry.path());
	}
	return files;
}

// What a report says of each file: how it was written and what it holds now.
std::vector<std::string> written_of(const pmg::migration_report& report)
{
	std::vector<std::string> out;
	for (const pmg::applied_file& file : report.files) {
		out.push_back(file.path + " " + file.written_as + " " + file.hash_after);
	}
	return out;
}

void prepare(const test_util::fixture_copy& copy)
{
	// Without the newer and broken files, as pmg_apply_tests: the migration runs (partial, module/unknown.desc stays).
	fs::remove(copy.res() / "newer.desc");
	fs::remove(copy.res() / "broken.desc");
}

// The main thread's side of on_main: a worker queues `fn` and waits; the test thread runs the queue while it waits for
// the apply to end, as the editor's frame callback does.
class main_hops
{
public:
	bool run(const std::function<void()>& fn)
	{
		std::promise<void> done;
		auto finished = done.get_future();
		{
			std::lock_guard lock(m_mutex);
			m_items.push_back(item{ &fn, &done });
		}
		m_queued.notify_one();
		finished.get();
		return true;
	}

	// Main thread: runs what is queued until `until` is ready.
	template<class T>
	void pump_until(std::future<T>& until, ds::task_system* tasks = nullptr)
	{
		while (until.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			if (tasks) tasks->drain_main(std::chrono::milliseconds(1));
			std::optional<item> next;
			{
				std::unique_lock lock(m_mutex);
				m_queued.wait_for(lock, std::chrono::milliseconds(5), [this] { return !m_items.empty(); });
				if (m_items.empty()) continue;
				next = m_items.front();
				m_items.pop_front();
			}
			(*next->fn)();
			++ran;
			next->done->set_value();
		}
	}

	std::atomic<int> ran{ 0 };

private:
	struct item
	{
		const std::function<void()>* fn = nullptr;
		std::promise<void>* done = nullptr;
	};

	std::mutex m_mutex;
	std::condition_variable m_queued;
	std::deque<item> m_items;
};

// scene_env over a copy of the fixture: manual tasks, the test thread is the main one.
struct manual_fixture : test_util::scene_env
{
	manual_fixture() { mount(copy); }

	void mount(const test_util::fixture_copy& project)
	{
		prepare(project);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ project.res() });
	}

	pmg::apply_context context_for(const fs::path& root)
	{
		pmg::apply_context context{ root, *descs, *rs, {}, {}, {}, {}, nullptr, {} };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	pmg::apply_outcome apply(const fs::path& root, const pmg::apply_context& context)
	{
		return pmg::apply_migration(pmg::plan_migration(root, pmg::scan_project(root, *descs), *descs), context);
	}

	test_util::fixture_copy copy{ "migration_v1" };
};

ds::task_system_config one_io_worker()
{
	ds::task_system_config config;
	config.mode = ds::policy::threaded;
	config.io_threads = 1;
	config.compute_threads = 2;
	return config;
}

// The editor's arrangement: a threaded pool with ONE io worker, the scene descs, res:// on a copy of the fixture.
struct threaded_env
{
	explicit threaded_env(const test_util::fixture_copy& project)
	{
		fs::create_directories("./res");
		fs::create_directories("./sandbox");
		prepare(project);
		rs = std::make_unique<res::resource_system>(tasks);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ project.res() });
		descs = std::make_unique<desc::desc_system>(*rs);
		rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs), std::ref(*descs));
		assembler = std::make_unique<scn::ecs_assembler>(*descs, *rs);
		scope.emplace(scn::register_scene_types(*descs, *assembler, skinning));
	}

	~threaded_env()
	{
		scope.reset();
		assembler.reset();
		descs.reset();
		rs.reset();
	}

	ds::task_system tasks{ one_io_worker() };
	std::unique_ptr<res::resource_system> rs;
	std::unique_ptr<desc::desc_system> descs;
	std::unique_ptr<scn::ecs_assembler> assembler;
	rnd::skinning_manager skinning;
	std::optional<scn::registration_scope> scope;
};

pmg::apply_context threaded_context(threaded_env& env, const fs::path& root)
{
	pmg::apply_context context{ root, *env.descs, *env.rs, {}, {}, {}, {}, nullptr, {} };
	context.make_id = [] { return std::string{ "test-migration" }; };
	return context;
}

pmg::migration_plan plan_of(threaded_env& env, const fs::path& root)
{
	return pmg::plan_migration(root, pmg::scan_project(root, *env.descs), *env.descs);
}

}

BOOST_AUTO_TEST_SUITE(PmgApplyOnMainTests)

BOOST_FIXTURE_TEST_CASE(OnMainThatRunsInline_GivesTheSameMigration_AndCarriesEveryEngineLoad, manual_fixture)
{
	test_util::fixture_copy plain{ "migration_v1" };
	mount(plain);
	const pmg::apply_outcome without = apply(plain.root(), context_for(plain.root()));
	BOOST_REQUIRE(without.refused.empty());

	mount(copy);
	int hops = 0;
	pmg::apply_context context = context_for(copy.root());
	context.on_main = [&hops](const std::function<void()>& fn) {
		++hops;
		fn();
		return true;
	};
	const pmg::apply_outcome with = apply(copy.root(), context);
	BOOST_REQUIRE(with.refused.empty());
	BOOST_TEST((with.report.status == without.report.status));
	BOOST_TEST(written_of(with.report) == written_of(without.report), boost::test_tools::per_element());
	BOOST_TEST((content_of(copy.root()) == content_of(plain.root())));
	// At least a form and a check for each file the engine wrote.
	std::size_t engine_files = 0;
	for (const pmg::applied_file& file : with.report.files) {
		engine_files += file.written_as == "engine" ? 1u : 0u;
	}
	BOOST_TEST(engine_files > 0u);
	BOOST_TEST(static_cast<std::size_t>(hops) >= 2u * engine_files);
}

BOOST_FIXTURE_TEST_CASE(OnMainThatRefuses_RollsTheMigrationBack, manual_fixture)
{
	const auto before = content_of(copy.root());
	pmg::apply_context context = context_for(copy.root());
	// Two hops go through and their file is written; the main thread is gone for the third.
	int hops = 0;
	context.on_main = [&hops](const std::function<void()>& fn) {
		if (++hops > 2) return false;
		fn();
		return true;
	};
	const pmg::apply_outcome outcome = apply(copy.root(), context);
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST(hops == 3);
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(outcome.report.error.find("main thread") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
	const json::value journal = json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST(journal.at("status").as_string() == "rolled_back");
	BOOST_TEST(pmg::unfinished_migrations(copy.root()).empty());
}

// The deadlock the editor must not have: the job's io worker waits for the main thread while the main thread waits for
// a load. A load runs on the vfs resolver's own thread and then on lane::compute (res_system.cpp require_block ->
// run_on_compute), never on lane::io, so the only io worker being blocked stalls nothing.
BOOST_AUTO_TEST_CASE(ARequireSyncOnMain_CompletesWhileTheOnlyIoWorkerWaitsForMain)
{
	test_util::fixture_copy project{ "migration_v1" };
	threaded_env env(project);
	std::promise<void> loaded;
	auto main_done = loaded.get_future().share();
	std::promise<void> io_started;
	auto started = io_started.get_future();
	auto job = env.tasks.run(ds::lane::io, ds::task_desc{ "blocked io", "test", false }, [&io_started, main_done](ds::task_context&) {
		io_started.set_value();
		main_done.wait();
	});
	BOOST_REQUIRE(started.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
	const auto camera = env.rs->require_sync<desc::desc_base>(res::tag::make("cameras/cam_v1.desc")).get_sync();
	loaded.set_value();
	BOOST_TEST(static_cast<bool>(camera));
	job.wait();
}

// The editor's run: apply_migration on the only io worker, its engine loads hopped to the test thread, which pumps them.
BOOST_AUTO_TEST_CASE(ApplyOnTheOnlyIoWorker_WithLoadsOnMain_GivesTheSameMigrationAsInline)
{
	test_util::fixture_copy inline_copy{ "migration_v1" };
	pmg::apply_outcome inline_outcome;
	{
		threaded_env env(inline_copy);
		inline_outcome = pmg::apply_migration(plan_of(env, inline_copy.root()), threaded_context(env, inline_copy.root()));
	}
	BOOST_REQUIRE(inline_outcome.refused.empty());

	test_util::fixture_copy project{ "migration_v1" };
	threaded_env env(project);
	const pmg::migration_plan plan = plan_of(env, project.root());
	main_hops hops;
	pmg::apply_context context = threaded_context(env, project.root());
	context.on_main = [&hops](const std::function<void()>& fn) { return hops.run(fn); };
	std::promise<pmg::apply_outcome> result;
	auto outcome = result.get_future();
	auto job = env.tasks.run(ds::lane::io, ds::task_desc{ "Migrate project", "project", false }, [&](ds::task_context&) {
		try {
			result.set_value(pmg::apply_migration(plan, context));
		} catch (...) {
			result.set_exception(std::current_exception());
		}
	});
	hops.pump_until(outcome, &env.tasks);
	const pmg::apply_outcome applied = outcome.get();
	job.wait();

	BOOST_REQUIRE(applied.refused.empty());
	BOOST_TEST_MESSAGE("error: " << applied.report.error);
	BOOST_TEST((applied.report.status == inline_outcome.report.status));
	BOOST_TEST(written_of(applied.report) == written_of(inline_outcome.report), boost::test_tools::per_element());
	BOOST_TEST((content_of(project.root()) == content_of(inline_copy.root())));
	BOOST_TEST(hops.ran.load() > 0);
}

BOOST_AUTO_TEST_SUITE_END()
