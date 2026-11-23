#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_model_file_access.h"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

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

BOOST_AUTO_TEST_CASE(ATextureThatIsNoImage_IsRefused_ByItsName_AndByItsBytesWhenRead)
{
	auto access = make();
	const auto by_name = access.resolve("notes.txt", edt::model_file_kind::texture);
	BOOST_CHECK(!by_name.error.empty());
	BOOST_CHECK(by_name.refusal == edt::refusal_kind::rule);
	// resolve() opens nothing (#1043): the bytes are checked by read(), on what it read.
	const auto by_bytes = access.resolve("fake.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(by_bytes.found());
	try {
		access.hash_listed(by_bytes);
		BOOST_ERROR("a texture that is no image was read");
	} catch (const std::runtime_error& e) {
		BOOST_CHECK(std::string(e.what()).find("no image") != std::string::npos);
	}
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
	const std::string many = few.resolve("tex/normal.png", edt::model_file_kind::texture).error;
	BOOST_CHECK_MESSAGE(many.find("more than 1 files (side files)") != std::string::npos, many);

	edt::model_file_limits small;
	small.max_total_bytes = png_bytes.size() + 1;
	auto budget = make(small);
	BOOST_CHECK(budget.resolve("skin.png", edt::model_file_kind::texture).found());
	const std::string total = budget.resolve("tex/normal.png", edt::model_file_kind::texture).error;
	BOOST_CHECK_MESSAGE(total.find("exceed " + edt::limit_text(small.max_total_bytes) + " in total (side files)") != std::string::npos, total);

	edt::model_file_limits tiny_file;
	tiny_file.max_file_bytes = 8;
	auto big = make(tiny_file);
	const std::string one = big.resolve("skin.png", edt::model_file_kind::texture).error;
	BOOST_CHECK_MESSAGE(one.find("exceeds 8 bytes per file (side files)") != std::string::npos, one);
}

BOOST_AUTO_TEST_CASE(TheSideFileLimits_Are64FilesOf64MiBAnd128MiBTogether_NamedInWholeUnits)
{
	const edt::model_file_limits limits;
	BOOST_CHECK_EQUAL(limits.max_files, 64u);
	BOOST_CHECK_EQUAL(edt::limit_text(limits.max_file_bytes), "64 MiB");
	BOOST_CHECK_EQUAL(edt::limit_text(limits.max_total_bytes), "128 MiB");
	BOOST_CHECK_EQUAL(edt::limit_text(64 * 1024), "64 KiB");
	BOOST_CHECK_EQUAL(edt::limit_text(2ull * 1024 * 1024 * 1024), "2 GiB");
	BOOST_CHECK_EQUAL(edt::limit_text(1025), "1025 bytes");
	BOOST_CHECK_EQUAL(edt::limit_text(0), "0 bytes");
	BOOST_CHECK_EQUAL(edt::duration_text(edt::k_decoder_run_time_limit), "2 min");
	BOOST_CHECK_EQUAL(edt::duration_text(edt::k_referenced_model_time_limit), "10 s");
	BOOST_CHECK_EQUAL(edt::duration_text(std::chrono::milliseconds(1500)), "1500 ms");
	BOOST_CHECK_EQUAL(edt::time_limit_refusal("decoding the model", edt::k_person_parse_time_limit),
		"decoding the model exceeds 2 min (decoder time)");
	BOOST_CHECK_EQUAL(edt::time_limit_refusal("reading the model", std::chrono::milliseconds::zero()),
		"reading the model exceeds its time limit (decoder time)");
}

BOOST_AUTO_TEST_CASE(ManyAliasesOfOneBigFile_CannotMakeTheImportReadItAgainAndAgain)
{
	edt::model_file_limits limits;
	limits.max_total_bytes = 3 * 12;
	auto scan = make(limits);
	const auto listed = scan.resolve("mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE(listed.found());
	scan.hash_listed(listed);
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

BOOST_AUTO_TEST_CASE(TheJob_RefusesFileWhoseBytesChanged_EvenAtTheSameSizeAndTime)
{
	auto scan = make();
	const auto listed = scan.resolve("mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE(listed.found());
	BOOST_CHECK(scan.read(listed).has_value());
	const std::vector<edt::referenced_file> allowed = scan.files();
	BOOST_REQUIRE(allowed.size() == 1u && allowed[0].hashed);

	const fs::path mesh = dir.path() / "models" / "mesh.bin";
	const auto time = fs::last_write_time(mesh);
	dir.write("models/mesh.bin", "buffer bytez");
	fs::last_write_time(mesh, time);

	auto job = make({}, &allowed);
	const auto file = job.resolve("mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE_MESSAGE(file.found(), file.error);
	BOOST_CHECK_THROW(job.read(file), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TheJob_RefusesATextureWhoseBytesChanged_EvenAtTheSameSizeAndTime)
{
	auto scan = make();
	const auto listed = scan.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(listed.found());
	scan.hash_listed(listed);
	scan.hash_listed(listed);
	const std::vector<edt::referenced_file> allowed = scan.files();
	BOOST_REQUIRE(allowed.size() == 1u && allowed[0].hashed);

	std::string changed = png_bytes;
	changed.back() = static_cast<char>(changed.back() ^ 0x01);
	const fs::path skin = dir.path() / "models" / "skin.png";
	const auto time = fs::last_write_time(skin);
	dir.write("models/skin.png", changed);
	fs::last_write_time(skin, time);

	auto job = make({}, &allowed);
	const auto file = job.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE_MESSAGE(file.found(), file.error);
	BOOST_CHECK_THROW(job.read(file), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TheJob_RefusesAListedFileWhoseBytesWereNeverHashed)
{
	auto scan = make();
	BOOST_REQUIRE(scan.resolve("skin.png", edt::model_file_kind::texture).found());
	const std::vector<edt::referenced_file> allowed = scan.files();
	BOOST_REQUIRE(allowed.size() == 1u && !allowed[0].hashed);

	auto job = make({}, &allowed);
	const auto file = job.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE_MESSAGE(file.found(), file.error);
	BOOST_CHECK_THROW(job.read(file), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(AFileWhoseIdCannotBeRead_IsNeverTrusted)
{
	auto scan = make();
	const auto listed = scan.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(listed.found());
	scan.hash_listed(listed);
	std::vector<edt::referenced_file> allowed = scan.files();
	allowed[0].id = edt::file_id{};

	auto job = make({}, &allowed);
	BOOST_CHECK(!job.resolve("skin.png", edt::model_file_kind::texture).error.empty());

	edt::resolved_reference nameless = listed;
	nameless.id = edt::file_id{};
	auto reader = make();
	BOOST_CHECK_THROW(reader.read(nameless), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TheJob_RefusesAnotherFileAtTheSamePathSizeAndTime)
{
	auto scan = make();
	BOOST_REQUIRE(scan.resolve("skin.png", edt::model_file_kind::texture).found());
	const std::vector<edt::referenced_file> allowed = scan.files();

	const fs::path skin = dir.path() / "models" / "skin.png";
	const auto time = fs::last_write_time(skin);
	dir.write("models/skin.next", png_bytes);
	fs::last_write_time(dir.path() / "models" / "skin.next", time);
	fs::rename(dir.path() / "models" / "skin.next", skin);

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

BOOST_AUTO_TEST_CASE(ABoundary_LetsTheModelReadFromItsContentRoot_ButNotBeyondItOrHiddenFiles)
{
	dir.write("content/models/robot.gltf", "{}");
	dir.write("content/models/near.png", png_bytes);
	dir.write("content/textures/skin.png", png_bytes);
	dir.write("content/.hidden/skin.png", png_bytes);
	edt::model_file_access access(dir.path() / "content" / "models" / "robot.gltf", {}, nullptr, dir.path() / "content");

	const auto shared = access.resolve("../textures/skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(shared.found());
	BOOST_CHECK_EQUAL(access.relative_name(shared), "textures/skin.png");
	const auto near = access.resolve("near.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(near.found());
	// Named from the boundary too: models/near.png and a textures/near.png can never share a name.
	BOOST_CHECK_EQUAL(access.relative_name(near), "models/near.png");

	const auto beyond = access.resolve("../../outside.png", edt::model_file_kind::texture);
	BOOST_CHECK(beyond.error.find("outside the content root of the model") != std::string::npos);
	BOOST_CHECK(!access.resolve("../.hidden/skin.png", edt::model_file_kind::texture).error.empty());
	BOOST_CHECK(!access.resolve((dir.path() / "outside.png").string(), edt::model_file_kind::texture).error.empty());
	BOOST_CHECK_EQUAL(access.files().size(), 2u);
}

BOOST_AUTO_TEST_CASE(ABoundaryThatDoesNotHoldTheModel_KeepsItToItsFolder)
{
	edt::model_file_access access(root, {}, nullptr, dir.path() / "elsewhere");
	BOOST_CHECK(access.resolve("skin.png", edt::model_file_kind::texture).found());
	const auto outside = access.resolve("../outside.png", edt::model_file_kind::texture);
	BOOST_CHECK(outside.error.find("outside the folder of the model") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AModelGivenOnlyAsBytes_ReadsNoOtherFile_EvenOneThatIsThere)
{
	// skin.png and mesh.bin lie next to the path: a model on the disk would find them (#925).
	edt::model_file_access access(root, {}, nullptr, {}, false);
	BOOST_CHECK(!access.on_disk());
	for (const auto& [reference, kind] : { std::pair{ "skin.png", edt::model_file_kind::texture }, std::pair{ "mesh.bin", edt::model_file_kind::side_file } }) {
		BOOST_TEST_CONTEXT(reference) {
			const auto refused = access.resolve(reference, kind);
			BOOST_CHECK(!refused.found());
			BOOST_CHECK(refused.error.find("not a file on the disk") != std::string::npos);
		}
	}
	BOOST_CHECK(access.files().empty());
	BOOST_CHECK_EQUAL(access.listed_bytes(), 0u);
}

BOOST_AUTO_TEST_CASE(ANameThatIsAShareADeviceOrAStream_IsRefusedByTheRule_BeforeTheDiskIsAsked)
{
	dir.write("models/m.obj", "o m");
	dir.write("models/x.mtl", "newmtl x");
	edt::model_file_access access(dir.path() / "models" / "m.obj", {});
	for (const char* reference : { "//host/share/x.mtl", "\\\\host\\share\\x.mtl", "\\\\?\\C:\\x.mtl", "\\\\.\\pipe\\x.mtl", "C:x.mtl",
			"x.mtl:stream", "sub/x.mtl:stream:$DATA" }) {
		BOOST_TEST_CONTEXT(reference) {
			const auto refused = access.resolve(reference, edt::model_file_kind::side_file);
			BOOST_CHECK(!refused.found());
			BOOST_CHECK(!refused.error.empty());
			BOOST_CHECK(refused.refusal == edt::refusal_kind::rule);
		}
	}
	BOOST_CHECK(!access.resolve(std::string("x.mtl\0.png", 10), edt::model_file_kind::side_file).error.empty());
	BOOST_CHECK(access.files().empty());
}

#if !defined(_WIN32)
// On Windows these names would reach a real share; on POSIX "//server/share" is a local path that does not exist.
BOOST_AUTO_TEST_CASE(AModelInAShareFolder_KeepsItsSideFiles_WhileAnotherShareIsRefused)
{
	edt::model_file_access access(fs::path("//server/share/models/m.obj"), {});
	const auto own = access.resolve("//server/share/models/m.mtl", edt::model_file_kind::side_file);
	BOOST_CHECK_MESSAGE(own.error.empty(), own.error);
	BOOST_CHECK(own.refusal == edt::refusal_kind::none);

	const auto other = access.resolve("//elsewhere/share/m.mtl", edt::model_file_kind::side_file);
	BOOST_CHECK(!other.found());
	BOOST_CHECK(other.refusal == edt::refusal_kind::rule);
	const auto stream = access.resolve("//server/share/models/m.mtl:stream", edt::model_file_kind::side_file);
	BOOST_CHECK(stream.refusal == edt::refusal_kind::rule);
}
#endif

BOOST_AUTO_TEST_CASE(AnAbsoluteSideFile_IsReadOnlyInsideTheModelFolder)
{
	dir.write("models/m.obj", "o m");
	dir.write("models/x.mtl", "newmtl x");
	dir.write("other.mtl", "newmtl y");
	edt::model_file_access access(dir.path() / "models" / "m.obj", {});
	const auto inside = access.resolve((dir.path() / "models" / "x.mtl").string(), edt::model_file_kind::side_file);
	BOOST_CHECK_MESSAGE(inside.found(), inside.error);
	const auto outside = access.resolve((dir.path() / "other.mtl").string(), edt::model_file_kind::side_file);
	BOOST_CHECK(!outside.found());
	BOOST_CHECK(outside.error.find("outside the folder of the model") != std::string::npos);
	BOOST_CHECK(outside.refusal == edt::refusal_kind::rule);
	const auto climbing = access.resolve((dir.path() / "models" / ".." / "other.mtl").string(), edt::model_file_kind::side_file);
	BOOST_CHECK(!climbing.found());
	BOOST_CHECK(climbing.refusal == edt::refusal_kind::rule);
}

// #1043: a relative name whose ".." leaves the folder is refused by its spelling, before anything asks the disk about it.
BOOST_AUTO_TEST_CASE(ARelativeNameThatClimbsOutOfTheFolder_IsRefusedByItsSpelling_AndOneThatStaysInsideResolves)
{
	dir.write("models/deep/m.obj", "o m");
	dir.write("models/deep/x.mtl", "newmtl x");
	dir.write("x.mtl", "newmtl y");
	edt::model_file_access access(dir.path() / "models" / "deep" / "m.obj", {});
	const auto climbing = access.resolve("../../x.mtl", edt::model_file_kind::side_file);
	BOOST_CHECK(!climbing.found());
	BOOST_CHECK(climbing.refusal == edt::refusal_kind::rule);
	BOOST_CHECK_MESSAGE(climbing.error.find("outside the folder of the model") != std::string::npos, climbing.error);
	const auto staying = access.resolve("sub/../x.mtl", edt::model_file_kind::side_file);
	BOOST_CHECK_MESSAGE(staying.found(), staying.error);
	BOOST_CHECK(access.resolve("..\\..\\x.mtl", edt::model_file_kind::side_file).refusal == edt::refusal_kind::rule);
}

BOOST_AUTO_TEST_CASE(ARefusal_SaysWhetherTheRuleALimitOrTheFileRefusedIt)
{
	BOOST_CHECK(make().resolve("../outside.png", edt::model_file_kind::texture).refusal == edt::refusal_kind::rule);
	BOOST_CHECK(make().resolve(".env", edt::model_file_kind::side_file).refusal == edt::refusal_kind::rule);
	edt::model_file_limits tiny;
	tiny.max_file_bytes = 4;
	BOOST_CHECK(make(tiny).resolve("mesh.bin", edt::model_file_kind::side_file).refusal == edt::refusal_kind::limit);
	fs::create_directories(dir.path() / "models" / "folder.png");
	BOOST_CHECK(make().resolve("folder.png", edt::model_file_kind::texture).refusal == edt::refusal_kind::other);
	BOOST_CHECK(make().resolve("skin.png", edt::model_file_kind::texture).refusal == edt::refusal_kind::none);
}

BOOST_AUTO_TEST_CASE(OneFile_MayBeNamedInAtMostEightWays)
{
	auto access = make();
	std::string name = "skin.png";
	for (std::size_t i = 0; i < edt::k_max_names_per_file; ++i) {
		BOOST_REQUIRE_MESSAGE(access.resolve(name, edt::model_file_kind::texture).found(), name);
		name = "./" + name;
	}
	const auto ninth = access.resolve(name, edt::model_file_kind::texture);
	BOOST_CHECK(!ninth.found());
	BOOST_CHECK(ninth.error.find("in more than 8 ways") != std::string::npos);
	BOOST_CHECK(ninth.refusal == edt::refusal_kind::limit);
	BOOST_CHECK_EQUAL(access.files().at(0).names.size(), edt::k_max_names_per_file);
	// A name it already has is no new one.
	BOOST_CHECK(access.resolve("skin.png", edt::model_file_kind::texture).found());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(AFifoInsteadOfATexture_IsNeverOpened)
{
	const fs::path fifo = dir.path() / "models" / "pipe.png";
	BOOST_REQUIRE_EQUAL(::mkfifo(fifo.c_str(), 0600), 0);
	auto access = make();
	// Nobody writes to the FIFO: opening it would block this test for good.
	const auto refused = access.resolve("pipe.png", edt::model_file_kind::texture);
	BOOST_CHECK(!refused.found());
	BOOST_CHECK(refused.error.find("not a plain file") != std::string::npos);
	BOOST_CHECK(refused.refusal == edt::refusal_kind::other);
	BOOST_CHECK(!access.read(refused).has_value());
	BOOST_CHECK(access.files().empty());
}
#endif

BOOST_AUTO_TEST_SUITE_END()

// The decoder process's side of the rule (#1043): it reads nothing from the disk, only what the editor handed over under
// the exact name the model asked for.
namespace {

std::shared_ptr<const std::vector<std::byte>> bytes_of(const std::string& text)
{
	auto bytes = std::make_shared<std::vector<std::byte>>(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) (*bytes)[i] = static_cast<std::byte>(text[i]);
	return bytes;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtModelFileAccessProvidedTests, access_fixture)

BOOST_AUTO_TEST_CASE(Recording_AnUnprovidedSideFileIsNotedOnce_AndIsNotFound_ThoughItLiesOnTheDisk)
{
	const std::vector<edt::provided_file> none;
	edt::model_file_access access(root, {}, &none, edt::model_file_access::provided_mode::record);
	BOOST_CHECK(access.provided());
	for (int i = 0; i < 2; ++i) {
		const auto asked = access.resolve("mesh.bin", edt::model_file_kind::side_file);
		BOOST_CHECK(!asked.found());
		BOOST_CHECK(asked.error.empty());
	}
	BOOST_CHECK(!access.resolve("skin.png", edt::model_file_kind::texture).found());
	BOOST_REQUIRE_EQUAL(access.requested().size(), 2u);
	BOOST_CHECK_EQUAL(access.requested()[0].name, "mesh.bin");
	BOOST_CHECK(access.requested()[0].kind == edt::model_file_kind::side_file);
	BOOST_CHECK(access.requested()[1].kind == edt::model_file_kind::texture);
	BOOST_CHECK(access.files().empty());
}

BOOST_AUTO_TEST_CASE(Recording_AProvidedFileIsServedUnderItsName_AndAMissingOneIsNeitherFoundNorAskedAgain)
{
	const std::vector<edt::provided_file> given = {
		{ "../shared/mesh.bin", "shared/mesh.bin", edt::model_file_kind::side_file, bytes_of("buffer bytes") },
		{ "gone.bin", "", edt::model_file_kind::side_file, nullptr } };
	edt::model_file_access access(root, {}, &given, edt::model_file_access::provided_mode::record);

	const auto bin = access.resolve("../shared/mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE(bin.found());
	BOOST_CHECK_EQUAL(bin.size, 12u);
	BOOST_CHECK_EQUAL(access.relative_name(bin), "shared/mesh.bin");
	const auto read = access.read(bin);
	BOOST_REQUIRE(read.has_value());
	BOOST_CHECK(*read == *given[0].bytes);

	const auto gone = access.resolve("gone.bin", edt::model_file_kind::side_file);
	BOOST_CHECK(!gone.found());
	BOOST_CHECK(gone.error.empty());
	// Only the exact name: the same file under another spelling was not handed over.
	BOOST_CHECK(!access.resolve("../shared/./mesh.bin", edt::model_file_kind::side_file).found());
	BOOST_REQUIRE_EQUAL(access.requested().size(), 1u);
	BOOST_CHECK_EQUAL(access.requested()[0].name, "../shared/./mesh.bin");
}

BOOST_AUTO_TEST_CASE(Strict_AnUnprovidedFileIsMissing_AndNothingIsReadForIt)
{
	// An .obj whose .mtl the scan found missing still imports, with a default material.
	const std::vector<edt::provided_file> none;
	edt::model_file_access access(root, {}, &none, edt::model_file_access::provided_mode::strict);
	const auto side = access.resolve("mesh.bin", edt::model_file_kind::side_file);
	BOOST_CHECK(!side.found());
	BOOST_CHECK(side.error.empty());
	BOOST_CHECK(!access.read(side).has_value());

	const auto texture = access.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_CHECK(!texture.found());
	BOOST_CHECK(texture.error.empty());
	BOOST_CHECK(!access.read(texture).has_value());
	BOOST_CHECK(access.requested().empty());
}

BOOST_AUTO_TEST_CASE(Strict_AFileAskedForAsAnotherKind_IsRefused_AndATextureThatIsNoImageThrowsOnRead)
{
	const std::vector<edt::provided_file> given = {
		{ "mesh.bin", "mesh.bin", edt::model_file_kind::side_file, bytes_of("buffer bytes") },
		{ "fake.png", "fake.png", edt::model_file_kind::texture, bytes_of("this is not a png") },
		{ "skin.png", "tex/skin.png", edt::model_file_kind::texture, bytes_of(png_bytes) } };
	edt::model_file_access access(root, {}, &given, edt::model_file_access::provided_mode::strict);

	const auto wrong = access.resolve("mesh.bin", edt::model_file_kind::texture);
	BOOST_CHECK(!wrong.found());
	BOOST_CHECK_MESSAGE(wrong.error.find("as another kind of file") != std::string::npos, wrong.error);

	const auto fake = access.resolve("fake.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(fake.found());
	BOOST_CHECK_THROW(access.read(fake), std::runtime_error);

	const auto skin = access.resolve("skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE(skin.found());
	BOOST_CHECK(access.read(skin).has_value());
	BOOST_CHECK_EQUAL(access.relative_name(skin), "tex/skin.png");
}

BOOST_AUTO_TEST_CASE(Strict_WhatIsReadIsBoundByTheLimitOfTheImport)
{
	const std::vector<edt::provided_file> given = { { "mesh.bin", "mesh.bin", edt::model_file_kind::side_file, bytes_of("buffer bytes") } };
	edt::model_file_limits limits;
	limits.max_total_bytes = 20;
	edt::model_file_access access(root, limits, &given, edt::model_file_access::provided_mode::strict);
	const auto bin = access.resolve("mesh.bin", edt::model_file_kind::side_file);
	BOOST_REQUIRE(bin.found());
	BOOST_CHECK(access.read(bin).has_value());
	BOOST_CHECK_THROW(access.read(bin), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(TheScan_RecordsEveryNameAFileWasAskedForBy_AndItsKind)
{
	auto access = make();
	BOOST_REQUIRE(access.resolve("skin.png", edt::model_file_kind::texture).found());
	BOOST_REQUIRE(access.resolve("./skin.png", edt::model_file_kind::texture).found());
	BOOST_REQUIRE(access.resolve("skin.png", edt::model_file_kind::texture).found());
	BOOST_REQUIRE(access.resolve("mesh.bin", edt::model_file_kind::side_file).found());
	BOOST_REQUIRE_EQUAL(access.files().size(), 2u);
	BOOST_CHECK((access.files()[0].names == std::vector<std::string>{ "skin.png", "./skin.png" }));
	BOOST_CHECK(access.files()[0].kind == edt::model_file_kind::texture);
	BOOST_CHECK((access.files()[1].names == std::vector<std::string>{ "mesh.bin" }));
	BOOST_CHECK(access.files()[1].kind == edt::model_file_kind::side_file);
}

BOOST_AUTO_TEST_SUITE_END()
