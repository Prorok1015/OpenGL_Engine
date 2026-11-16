#include <boost/test/unit_test.hpp>
#include "edt_decoder_sandbox_notice.h"
#include <snk/editor/edt_problem_list.h>
#include <string>

// #1076: a decoder without its OS sandbox never fails an import, but a person sees it in Problems.

namespace {

edt::decoder_sandbox_status status(bool fs, bool syscalls, std::string why = {})
{
	edt::decoder_sandbox_status reported;
	reported.reported = true;
	reported.fs = fs;
	reported.syscalls = syscalls;
	reported.why = std::move(why);
	return reported;
}

const edt::decoder_sandbox_status k_whole = status(true, true);

std::string decoder_row(const edt::problem_list& problems)
{
	for (const edt::sourced_problem& row : problems.list()) {
		if (row.source == edt::decoder_sandbox_notice::source) return row.item.message;
	}
	return {};
}

}

BOOST_AUTO_TEST_SUITE(EdtDecoderSandboxNoticeTests)

BOOST_AUTO_TEST_CASE(ADecoderWithoutLandlock_IsAWarningInProblems_ThatSaysTheFilesAreNotProtected)
{
	edt::problem_list problems;
	edt::decoder_sandbox_notice notice(problems, edt::sandbox_mode::best_effort, k_whole);
	BOOST_CHECK(decoder_row(problems).empty());

	notice.seen(status(false, true, "the kernel has no usable Landlock"));
	const std::string row = decoder_row(problems);
	BOOST_CHECK_MESSAGE(row.find("without OS isolation") != std::string::npos, row);
	BOOST_CHECK_MESSAGE(row.find("the kernel has no usable Landlock") != std::string::npos, row);
	BOOST_CHECK_MESSAGE(row.find("every file") != std::string::npos, row);
	BOOST_CHECK_MESSAGE(row.find("import/decoder_sandbox") != std::string::npos, row);
	BOOST_REQUIRE_EQUAL(problems.size(), 1u);
	BOOST_CHECK(problems.list().front().item.severity == edt::problem_severity::warning);
}

BOOST_AUTO_TEST_CASE(ADecoderInItsWholeSandbox_LeavesProblemsEmpty)
{
	edt::problem_list problems;
	edt::decoder_sandbox_notice notice(problems, edt::sandbox_mode::best_effort, k_whole);
	notice.seen(k_whole);
	BOOST_CHECK_EQUAL(problems.size(), 0u);
	// A status the child never gave (another importer, a fake) says nothing either way.
	notice.seen(edt::decoder_sandbox_status{});
	BOOST_CHECK_EQUAL(problems.size(), 0u);
}

BOOST_AUTO_TEST_CASE(TheWorstGapOfTheRun_StaysUntilTheEditorRestarts)
{
	edt::problem_list problems;
	edt::decoder_sandbox_notice notice(problems, edt::sandbox_mode::best_effort, k_whole);
	notice.seen(status(true, false, "the kernel has no seccomp"));
	BOOST_CHECK(decoder_row(problems).find("system call filter") != std::string::npos);

	notice.seen(status(false, false, "no Landlock"));
	BOOST_CHECK(decoder_row(problems).find("without OS isolation") != std::string::npos);
	notice.seen(k_whole);
	BOOST_CHECK(decoder_row(problems).find("without OS isolation") != std::string::npos);
	BOOST_CHECK(!notice.current().fs);
	BOOST_CHECK_EQUAL(problems.size(), 1u);
}

BOOST_AUTO_TEST_CASE(ASandboxThePersonTurnedOff_IsInProblemsFromTheStart)
{
	edt::problem_list problems;
	edt::decoder_sandbox_notice notice(problems, edt::sandbox_mode::off, edt::expected_decoder_sandbox(edt::sandbox_mode::off));
	const std::string row = decoder_row(problems);
	BOOST_CHECK_MESSAGE(row.find("turned off") != std::string::npos, row);
	BOOST_CHECK_MESSAGE(row.find("every file") != std::string::npos, row);
}

BOOST_AUTO_TEST_CASE(BeforeAnyRun_TheExpectationIsWhatAQuestionShows)
{
	edt::problem_list problems;
	const edt::decoder_sandbox_status expected = status(false, false, "this system has no sandbox for the decoder yet");
	edt::decoder_sandbox_notice notice(problems, edt::sandbox_mode::best_effort, expected);
	// Only a decoder's own word puts a row in Problems; the expectation is for a question shown before it ran.
	BOOST_CHECK_EQUAL(problems.size(), 0u);
	BOOST_CHECK(!notice.current().fs);
	BOOST_CHECK_EQUAL(notice.current().why, expected.why);
}

BOOST_AUTO_TEST_CASE(TheExpectation_OfAModeTurnedOff_IsNoSandbox_AndOtherwiseNamesWhatIsMissing)
{
	const edt::decoder_sandbox_status off = edt::expected_decoder_sandbox(edt::sandbox_mode::off);
	BOOST_CHECK(off.reported && !off.fs && !off.syscalls);
	BOOST_CHECK_EQUAL(edt::decoder_sandbox_gap(off), 2);

	const edt::decoder_sandbox_status on = edt::expected_decoder_sandbox(edt::sandbox_mode::best_effort);
	BOOST_CHECK(on.reported);
	BOOST_CHECK_MESSAGE((edt::decoder_sandbox_gap(on) == 0) == on.why.empty(), on.why);
#if !defined(__linux__)
	BOOST_CHECK(!on.fs);
#endif
}

BOOST_AUTO_TEST_SUITE_END()
