#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_model_file_access.h"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

// The files a model may make an agent's import read (#548): its folder without hidden names, images checked by their
// bytes, a bounded number of files and bytes, and only what the person was shown.

namespace {

namespace fs = std::filesystem;

const std::string png_bytes = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, 'x');

struct access_fixture
{
	access_fixture()
	{
		dir.write("models/robot.gltf", "{}");
		dir.write("models/skin.png", png_bytes);
		dir.write("models/tex/normal.png", png_bytes);
		dir.write("models/mesh.bin", "buffer bytes");
		dir.write("models/.secret/skin.png", png_bytes);
		dir.write("models/.env", "TOKEN=1");
		dir.write("models/notes.txt", "not an image");
		dir.write("models/fake.png", "this is not a png");
		dir.write("outside.png", png_bytes);
		root = dir.path() / "models" / "robot.gltf";
	}

	edt::model_file_access make(edt::model_file_limits limits = {}, const std::vector<edt::referenced_file>* allowed = nullptr)
	{
		return edt::model_file_access(root, limits, allowed);
	}

	test_util::temp_dir dir{ "model_file_access" };
	fs::path root;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtModelFileAccessTests, access_fixture)

BOOST_AUTO_TEST_CASE(AnImageInTheModelFolderOrBelow_IsListedWithItsSize)
{
	auto access = make();
	BOOST_CHECK(access.resolve("skin.png", edt::model_file_kind::texture).found());
	BOOST_CHECK(access.resolve("tex/normal.png", edt::model_file_kind::texture).found());
	BOOST_REQUIRE_EQUAL(access.files().size(), 2u);
	BOOST_CHECK_EQUAL(access.files()[0].size, png_bytes.size());
	BOOST_CHECK_EQUAL(access.listed_bytes(), 2 * png_bytes.size());
}

BOOST_AUTO_TEST_CASE(AMissingTexture_IsNeitherListedNorAnError)
{
	auto access = make();
	const auto missing = access.resolve("nowhere.png", edt::model_file_kind::texture);
	BOOST_CHECK(!missing.found());
	BOOST_CHECK(missing.error.empty());
	BOOST_CHECK(access.files().empty());
}

BOOST_AUTO_TEST_CASE(HiddenFilesAndFolders_AreRefused)
{
	auto access = make();
	for (const char* reference : { ".env", ".secret/skin.png", "tex/../.secret/skin.png", ".ssh/id_rsa" }) {
		BOOST_TEST_CONTEXT(reference) {
			const auto refused = access.resolve(reference, edt::model_file_kind::texture);
			BOOST_CHECK(!refused.found());
			BOOST_CHECK(!refused.error.empty());
		}
	}
	BOOST_CHECK(access.files().empty());
}

BOOST_AUTO_TEST_CASE(AnythingOutsideTheModelFolder_IsRefused)
{
	auto access = make();
	for (const char* reference : { "../outside.png", "tex/../../outside.png", "..\\outside.png" }) {
		BOOST_TEST_CONTEXT(reference) { BOOST_CHECK(!access.resolve(reference, edt::model_file_kind::texture).error.empty()); }
	}
	BOOST_CHECK(!access.resolve((dir.path() / "outside.png").string(), edt::model_file_kind::texture).error.empty());
	BOOST_CHECK(access.files().empty());
}

BOOST_AUTO_TEST_CASE(ATextureThatIsNoImage_IsRefused_ByItsNameAndByItsBytes)
{
	auto access = make();
	const auto by_name = access.resolve("notes.txt", edt::model_file_kind::texture);
	BOOST_CHECK(!by_name.error.empty());
	const auto by_bytes = access.resolve("fake.png", edt::model_file_kind::texture);
	BOOST_CHECK(!by_bytes.error.empty());
	BOOST_CHECK(by_bytes.error.find("no image") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ASideFile_IsABinBesideAGltf_NothingElse)
{
	auto access = make();
	BOOST_CHECK(access.resolve("mesh.bin", edt::model_file_kind::side_file).found());
	BOOST_CHECK(!access.resolve("skin.png", edt::model_file_kind::side_file).error.empty());
	BOOST_CHECK(!access.resolve("notes.txt", edt::model_file_kind::side_file).error.empty());
	BOOST_CHECK(!access.resolve(".env", edt::model_file_kind::side_file).error.empty());
}

BOOST_AUTO_TEST_CASE(TheSameFileUnderManySpellings_IsListedAndCountedOnce)
{
	auto access = make();
	for (const char* reference : { "skin.png", "./skin.png", "tex/../skin.png", "tex/../tex/../skin.png" }) {
		BOOST_CHECK(access.resolve(reference, edt::model_file_kind::texture).found());
	}
	BOOST_CHECK_EQUAL(access.files().size(), 1u);
	BOOST_CHECK_EQUAL(access.listed_bytes(), png_bytes.size());
	const auto one = access.resolve("skin.png", edt::model_file_kind::texture);
	const auto other = access.resolve("tex/../skin.png", edt::model_file_kind::texture);
	BOOST_CHECK_EQUAL(access.relative_name(one), access.relative_name(other));
}

BOOST_AUTO_TEST_CASE(TooManyFilesOrTooManyBytes_RefuseTheModel)
{
	edt::model_file_limits two_files;
	two_files.max_files = 1;
	auto few = make(two_files);
	BOOST_CHECK(few.resolve("skin.png", edt::model_file_kind::texture).found());
	BOOST_CHECK(!few.resolve("tex/normal.png", edt::model_file_kind::texture).error.empty());

	edt::model_file_limits small;
	small.max_total_bytes = png_bytes.size() + 1;
	auto budget = make(small);
	BOOST_CHECK(budget.resolve("skin.png", edt::model_file_kind::texture).found());
	BOOST_CHECK(!budget.resolve("tex/normal.png", edt::model_file_kind::texture).error.empty());

	edt::model_file_limits tiny_file;
	tiny_file.max_file_bytes = 8;
	auto big = make(tiny_file);
	BOOST_CHECK(!big.resolve("skin.png", edt::model_file_kind::texture).error.empty());
}

BOOST_AUTO_TEST_CASE(ManyAliasesOfOneBigFile_CannotMakeTheImportReadItAgainAndAgain)
{
	edt::model_file_limits limits;
	limits.max_total_bytes = 3 * 12;
	auto scan = make(limits);
	const auto listed = scan.resolve("mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE(listed.found());
	std::vector<edt::referenced_file> allowed = scan.files();

	auto job = make(limits, &allowed);
	const auto file = job.resolve("tex/../mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE(file.found());
	BOOST_CHECK(job.read(file).has_value());
	BOOST_CHECK(job.read(file).has_value());
	BOOST_CHECK(job.read(file).has_value());
	BOOST_CHECK_THROW(job.read(file), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TheJob_ReadsOnlyWhatWasShown_AtTheSizeItWasShown)
{
	auto scan = make();
	BOOST_REQUIRE(scan.resolve("skin.png", edt::model_file_kind::texture).found());
	std::vector<edt::referenced_file> allowed = scan.files();

	auto job = make({}, &allowed);
	BOOST_CHECK(job.resolve("skin.png", edt::model_file_kind::texture).found());
	const auto other = job.resolve("tex/normal.png", edt::model_file_kind::texture);
	BOOST_CHECK(!other.found());
	BOOST_CHECK(other.error.find("shown") != std::string::npos);

	dir.write("models/skin.png", png_bytes + "longer now");
	auto changed = make({}, &allowed);
	BOOST_CHECK(!changed.resolve("skin.png", edt::model_file_kind::texture).error.empty());
}

BOOST_AUTO_TEST_CASE(TheJob_RefusesAFileWhoseTimeChanged_EvenAtTheSameSize)
{
	auto scan = make();
	BOOST_REQUIRE(scan.resolve("skin.png", edt::model_file_kind::texture).found());
	std::vector<edt::referenced_file> allowed = scan.files();

	fs::last_write_time(dir.path() / "models" / "skin.png", fs::file_time_type::clock::now() + std::chrono::hours(1));
	auto job = make({}, &allowed);
	BOOST_CHECK(!job.resolve("skin.png", edt::model_file_kind::texture).error.empty());
}

BOOST_AUTO_TEST_CASE(TheBytesThatAreRead_AreCheckedAgainAsAnImage)
{
	auto access = make();
	const auto skin = access.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(skin.found());
	BOOST_CHECK(access.read(skin).has_value());

	edt::resolved_reference swapped = skin;
	swapped.path = dir.path() / "models" / "fake.png";
	swapped.size = fs::file_size(swapped.path);
	swapped.modified = fs::last_write_time(swapped.path);
	BOOST_CHECK_THROW(access.read(swapped), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
