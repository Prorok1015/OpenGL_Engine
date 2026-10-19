#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#include <snk/render/rnd_render_service_init.h>
#include <filesystem>
#include <fstream>
#include <iterator>

// Every .desc the repository ships (core/res + Editor/res layers) must load, be a fixed point of the engine's own save, and
// carry the current __version on every typed object. When a format changes, run tools/desc_resave on
// core/res and Editor/res: this test fails on files that are still in an older format.

namespace json = boost::json;
namespace fs = std::filesystem;

#ifndef SNK_EDITOR_RES_DIR
#error "SNK_EDITOR_RES_DIR must point at Editor/res"
#endif
#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

struct repo_file {
	std::string rel;
	std::vector<std::byte> bytes;
	bool is_desc = false;
};

// Read once: the images next to the descs (skybox faces) are loaded by descs that reference them.
const std::vector<repo_file>& repo_files()
{
	static const std::vector<repo_file> files = [] {
		std::vector<repo_file> out;
		// Base layer first; a file of the editor layer replaces the base file at the same path.
		for (const fs::path& root : { fs::path{ SNK_CORE_RES_DIR }, fs::path{ SNK_EDITOR_RES_DIR } }) {
			for (const auto& entry : fs::recursive_directory_iterator(root)) {
				if (!entry.is_regular_file()) continue;
				std::ifstream in(entry.path(), std::ios::binary);
				std::vector<char> text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
				repo_file file;
				file.rel = fs::relative(entry.path(), root).generic_string();
				file.bytes.resize(text.size());
				std::memcpy(file.bytes.data(), text.data(), text.size());
				file.is_desc = entry.path().extension() == ".desc";
				std::erase_if(out, [&](const repo_file& f) { return f.rel == file.rel; });
				out.push_back(std::move(file));
			}
		}
		std::ranges::sort(out, {}, &repo_file::rel);
		return out;
	}();
	return files;
}

struct repo_fixture : test_util::scene_env
{
	repo_fixture()
		: adapter(*rs, *descs)
	{
		engine::render::register_render_descs(*descs);
		// Levels reference models that are imported into prefabs while they load.
		rs->registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(*descs), std::ref(*rs));
		// Serve the repository's res:// files from RAM so the test never touches the working directory.
		rs->unregistrate_resolver(res::tag::default_protocol());
		auto& ram_res = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
		for (const auto& file : repo_files()) {
			ram_res.files[res::tag::make(file.rel)] = file.bytes;
		}
	}

	~repo_fixture()
	{
		engine::render::unregister_render_descs(*descs);
	}

	template<class Fn>
	void for_each_desc(Fn&& fn)
	{
		std::size_t count = 0;
		for (const auto& file : repo_files()) {
			if (!file.is_desc) continue;
			++count;
			BOOST_TEST_CONTEXT("res://" << file.rel) {
				fn(res::tag::make(file.rel), file);
			}
		}
		BOOST_TEST(count > 0u);
	}

	std::shared_ptr<desc::desc_base> load_desc(const res::tag& tag)
	{
		return require_loaded<desc::desc_base>(tag).get_sync();
	}

	static json::value parse(const std::vector<std::byte>& bytes)
	{
		return json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	}

	// Every object with a string __type, anywhere in the tree.
	void check_versions(const json::value& value)
	{
		if (value.is_object()) {
			const auto& obj = value.as_object();
			if (auto type = obj.if_contains("__type"); type && type->is_string()) {
				const int expected = descs->current_version(std::string{ type->as_string() });
				auto version = obj.if_contains("__version");
				BOOST_TEST_REQUIRE((version && version->is_int64()), "'" << type->as_string() << "' has no __version");
				BOOST_TEST(version->as_int64() == expected, "'" << type->as_string() << "'");
			}
			for (const auto& member : obj) check_versions(member.value());
		}
		else if (value.is_array()) {
			for (const auto& element : value.as_array()) check_versions(element);
		}
	}

	core::desc::desc_adapter_t adapter;
};

}

BOOST_FIXTURE_TEST_SUITE(DescRepoRoundtripTests, repo_fixture)

BOOST_AUTO_TEST_CASE(EveryRepositoryDescLoads)
{
	for_each_desc([this](const res::tag& tag, const repo_file&) {
		BOOST_CHECK(load_desc(tag) != nullptr);
	});
}

BOOST_AUTO_TEST_CASE(SavingALoadedFileGivesTheFileBack)
{
	for_each_desc([this](const res::tag& tag, const repo_file& file) {
		auto loaded = load_desc(tag);
		BOOST_REQUIRE(loaded);
		const auto saved = parse(adapter.serialize(tag, loaded));
		BOOST_CHECK_MESSAGE(saved == parse(file.bytes), "run tools/desc_resave on core/res and Editor/res");
	});
}

BOOST_AUTO_TEST_CASE(SaveIsIdempotent)
{
	for_each_desc([this](const res::tag& tag, const repo_file&) {
		auto loaded = load_desc(tag);
		BOOST_REQUIRE(loaded);
		const auto first = adapter.serialize(tag, loaded);
		auto reloaded = adapter.deserialize(tag, first);
		BOOST_REQUIRE(reloaded);
		BOOST_CHECK(parse(adapter.serialize(tag, reloaded)) == parse(first));
	});
}

BOOST_AUTO_TEST_CASE(EveryTypedObjectHasItsCurrentVersion)
{
	for_each_desc([this](const res::tag&, const repo_file& file) {
		check_versions(parse(file.bytes));
	});
}

BOOST_AUTO_TEST_CASE(SavedFilesAreIndentedAndEndWithANewline)
{
	for_each_desc([](const res::tag&, const repo_file& file) {
		BOOST_CHECK(file.bytes.size() > 2u);
		const std::string text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
		BOOST_CHECK(text.find('\n') != std::string::npos);
		BOOST_CHECK(text.back() == '\n');
	});
}

BOOST_AUTO_TEST_SUITE_END()
