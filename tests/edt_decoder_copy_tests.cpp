#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_decoder_copy.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// #1054: the decoder and exactly the DLLs it loads from its own folder, copied once per set of files for its AppContainer.

namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct copy_fixture
{
	copy_fixture()
	{
		decoder = dir.write("build/snk_model_decoder.exe", "the decoder");
		dir.write("build/snk_model_decoder.deps", "snk_engine.dll\r\nboost_json.dll\n\n");
		dir.write("build/snk_engine.dll", "the engine");
		dir.write("build/boost_json.dll", "json");
		dir.write("build/vcruntime140.dll", "the runtime");
		dir.write("build/editor.exe", "not the decoder's");
		dir.write("build/game.dll", "not listed");
	}

	test_util::temp_dir dir{ "decoder_copy" };
	fs::path decoder;
	const fs::path base = dir.path() / "copies";
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDecoderCopyTests, copy_fixture)

BOOST_AUTO_TEST_CASE(TheCopyHoldsTheDecoder_TheListedDlls_AndTheRuntime_AndNothingElse)
{
	std::string error;
	const auto copy = edt::make_decoder_copy(decoder, base, error);
	BOOST_REQUIRE_MESSAGE(copy.has_value(), error);
	BOOST_CHECK(copy->program == copy->folder / "snk_model_decoder.exe");
	BOOST_CHECK(copy->folder.parent_path() == base / "snk_model_decoder");
	BOOST_CHECK_EQUAL(read_text(copy->program), "the decoder");
	BOOST_CHECK_EQUAL(read_text(copy->folder / "snk_engine.dll"), "the engine");
	BOOST_CHECK_EQUAL(read_text(copy->folder / "boost_json.dll"), "json");
	BOOST_CHECK_EQUAL(read_text(copy->folder / "vcruntime140.dll"), "the runtime");
	BOOST_CHECK(!fs::exists(copy->folder / "editor.exe"));
	BOOST_CHECK(!fs::exists(copy->folder / "game.dll"));
	BOOST_CHECK(!fs::exists(copy->folder / "snk_model_decoder.deps"));
}

BOOST_AUTO_TEST_CASE(TheSameSet_IsCopiedOnce_AnotherSetGetsAnotherFolder_AndTheOldOneGoes)
{
	std::string error;
	const auto first = edt::make_decoder_copy(decoder, base, error);
	BOOST_REQUIRE_MESSAGE(first.has_value(), error);
	const auto again = edt::make_decoder_copy(decoder, base, error);
	BOOST_REQUIRE_MESSAGE(again.has_value(), error);
	BOOST_CHECK(again->folder == first->folder);

	// Another engine with the same decoder is another set: no old DLL stays beside the new decoder.
	dir.write("build/snk_engine.dll", "the next engine");
	const auto next = edt::make_decoder_copy(decoder, base, error);
	BOOST_REQUIRE_MESSAGE(next.has_value(), error);
	BOOST_CHECK(next->folder != first->folder);
	BOOST_CHECK_EQUAL(read_text(next->folder / "snk_engine.dll"), "the next engine");
	BOOST_CHECK(!fs::exists(first->folder));
}

BOOST_AUTO_TEST_CASE(AChangedFileInTheCopy_IsReplaced)
{
	std::string error;
	const auto copy = edt::make_decoder_copy(decoder, base, error);
	BOOST_REQUIRE_MESSAGE(copy.has_value(), error);
	{
		std::ofstream tampered(copy->folder / "snk_engine.dll", std::ios::binary | std::ios::trunc);
		tampered << "planted";
	}
	const auto again = edt::make_decoder_copy(decoder, base, error);
	BOOST_REQUIRE_MESSAGE(again.has_value(), error);
	BOOST_CHECK_EQUAL(read_text(again->folder / "snk_engine.dll"), "the engine");
}

BOOST_AUTO_TEST_CASE(AListThatLeavesTheFolder_OrNamesAMissingFile_MakesNoCopy)
{
	std::string error;
	dir.write("build/snk_model_decoder.deps", "../outside.dll\n");
	BOOST_CHECK(!edt::make_decoder_copy(decoder, base, error).has_value());
	BOOST_CHECK_MESSAGE(error.find("outside.dll") != std::string::npos, error);

	error.clear();
	dir.write("build/snk_model_decoder.deps", "missing.dll\n");
	BOOST_CHECK(!edt::make_decoder_copy(decoder, base, error).has_value());
	BOOST_CHECK_MESSAGE(error.find("missing.dll") != std::string::npos, error);

	error.clear();
	fs::remove(dir.path() / "build" / "snk_model_decoder.deps");
	BOOST_CHECK(!edt::make_decoder_copy(decoder, base, error).has_value());
	BOOST_CHECK_MESSAGE(error.find(".deps") != std::string::npos, error);

	error.clear();
	BOOST_CHECK(!edt::make_decoder_copy(decoder, fs::path{}, error).has_value());
	BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(OnlyTheExitCodesOfALoaderThatFailed_SayTheDecoderCouldNotLoad)
{
	BOOST_CHECK(edt::is_loader_failure(static_cast<int>(0xC0000135u)));
	BOOST_CHECK(edt::is_loader_failure(static_cast<int>(0xC0000022u)));
	BOOST_CHECK(!edt::is_loader_failure(0));
	BOOST_CHECK(!edt::is_loader_failure(4));
	BOOST_CHECK(!edt::is_loader_failure(-1));
}

#if defined(_WIN32) && defined(SNK_MODEL_DECODER_PATH)
// The decoder this build made, with its runtime: the log line of make_decoder_copy is the measurement of #1158.
BOOST_AUTO_TEST_CASE(TheCopyOfTheBuiltDecoder_IsMade)
{
	std::string error;
	const auto copy = edt::make_decoder_copy(SNK_MODEL_DECODER_PATH, dir.path() / "built", error);
	BOOST_REQUIRE_MESSAGE(copy.has_value(), error);
	BOOST_CHECK(fs::is_regular_file(copy->program));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
