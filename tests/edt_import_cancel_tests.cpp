#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>

// The import and export job bodies stop at their safe points when the job is cancelled (EDITOR_FRAMEWORK, job_service).

namespace {

constexpr const char* OBJ_TRIANGLE = R"(v -1 -1 0
v 1 -1 0
v 0 1 0
o Tri
f 1 2 3
)";

// Asks for a stop right after the first store: the material is the first thing a model import stores, so the next
// checkpoint after the prefab is built finds the job cancelled with tags already in the resolver.
class stopping_resolver : public test_util::ram_resolver
{
public:
	stopping_resolver(const std::string_view prt, std::stop_source* source) : ram_resolver(prt), m_source(source) {}

	bool store(const res::tag& tag, const std::vector<std::byte>& data) override
	{
		const bool stored = ram_resolver::store(tag, data);
		if (++stores == 1) {
			m_source->request_stop();
		}
		return stored;
	}

	int stores = 0;

private:
	std::stop_source* m_source;
};

struct cancel_fixture : test_util::scene_env
{
	cancel_fixture()
	{
		dir = std::filesystem::temp_directory_path() / "snk_import_cancel_tests";
		std::filesystem::create_directories(dir);
		model = dir / "tri.obj";
		std::ofstream(model) << OBJ_TRIANGLE;
	}

	~cancel_fixture()
	{
		std::error_code ignored;
		std::filesystem::remove_all(dir, ignored);
	}

	static ds::task_context context_of(const std::stop_token& token)
	{
		ds::task_context ctx;
		ctx.stop = token;
		return ctx;
	}

	std::filesystem::path dir;
	std::filesystem::path model;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtImportCancelTests, cancel_fixture)

BOOST_AUTO_TEST_CASE(Import_StoppedBeforeItStarts_ThrowsCancelledAndStoresNothing)
{
	edt::model_importer importer(*descs, *rs);
	std::stop_source source;
	source.request_stop();
	ds::task_context ctx = context_of(source.get_token());

	BOOST_CHECK_THROW(importer.import_background(model, &ctx), ds::task_cancelled);
	BOOST_TEST(ram->files.empty());
}

BOOST_AUTO_TEST_CASE(Import_StoppedAfterTheMaterialPhase_LeavesNoMemoryTagBehind)
{
	std::stop_source source;
	rs->unregistrate_resolver(res::tag::memory);
	auto& stopping = rs->registrate_resolver<stopping_resolver>(res::tag::memory, &source);
	edt::model_importer importer(*descs, *rs);
	ds::task_context ctx = context_of(source.get_token());

	BOOST_CHECK_THROW(importer.import_background(model, &ctx), ds::task_cancelled);

	BOOST_TEST(stopping.stores >= 1); // something was stored before the stop
	BOOST_TEST(stopping.files.empty());
}

BOOST_AUTO_TEST_CASE(Import_WithAContextThatIsNotStopped_RunsToTheEnd)
{
	edt::model_importer importer(*descs, *rs);
	std::stop_source source;
	ds::task_context ctx = context_of(source.get_token());

	const edt::import_intermediate result = importer.import_background(model, &ctx);

	BOOST_TEST(result.error.empty());
	BOOST_TEST(result.prefab_tag.is_valid());
	BOOST_TEST(!ram->files.empty());
}

BOOST_AUTO_TEST_CASE(Import_WithoutAContext_StillWorks)
{
	edt::model_importer importer(*descs, *rs);

	const edt::import_intermediate result = importer.import_background(model);

	BOOST_TEST(result.error.empty());
}

BOOST_AUTO_TEST_CASE(Export_StoppedBeforeTheFirstFile_ThrowsCancelled)
{
	edt::asset_exporter exporter(*rs);
	edt::import_result imported;
	imported.source_path = "tri.obj";
	imported.root_tag = res::tag{ res::tag::memory, "tri/tri.prefab.desc" };
	imported.all_tags.push_back(imported.root_tag);
	rs->store(imported.root_tag, test_util::to_bytes("{}"));
	const auto remap = exporter.build_remap(imported, "tri", "tests/cancel");
	std::stop_source source;
	source.request_stop();
	ds::task_context ctx = context_of(source.get_token());
	edt::export_progress progress;

	BOOST_CHECK_THROW(exporter.export_to_project(imported, remap, &progress, &ctx), ds::task_cancelled);
	BOOST_TEST(progress.done.load() == 0);
	BOOST_TEST(!progress.success.load());
}

BOOST_AUTO_TEST_CASE(Export_WithNothingToWrite_FinishesWhenNotStopped)
{
	edt::asset_exporter exporter(*rs);
	std::stop_source source;
	ds::task_context ctx = context_of(source.get_token());
	edt::export_progress progress;

	BOOST_TEST(exporter.export_to_project(edt::import_result{}, {}, &progress, &ctx));
	BOOST_TEST(progress.success.load());
}

BOOST_AUTO_TEST_SUITE_END()
