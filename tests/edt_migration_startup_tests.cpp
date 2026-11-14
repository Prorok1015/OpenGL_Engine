#include <boost/test/unit_test.hpp>
#include "edt_migration_startup.h"
#include <string>
#include <vector>

// What the editor does about a migration a project was left with when it opens (#761): a person is asked, headless a
// WARN names the command line; the id is shown without control characters.
BOOST_AUTO_TEST_SUITE(EdtMigrationStartup)

BOOST_AUTO_TEST_CASE(NothingUnfinished_OffersNothing)
{
	BOOST_TEST((edt::decide_journal_offer({}, false).what == edt::journal_offer::kind::none));
	BOOST_TEST((edt::decide_journal_offer({}, true).what == edt::journal_offer::kind::none));
}

BOOST_AUTO_TEST_CASE(APerson_IsAskedAboutTheFirst_HeadlessGetsTheCommandLine)
{
	const std::vector<std::string> unfinished{ "20260101T000000Z-0.1.0", "20260202T000000Z-0.2.0" };
	const edt::journal_offer asked = edt::decide_journal_offer(unfinished, false);
	BOOST_TEST((asked.what == edt::journal_offer::kind::ask));
	BOOST_TEST(asked.id == unfinished.front());
	BOOST_TEST(asked.title == "Migration 20260101T000000Z-0.1.0 did not finish");
	BOOST_TEST(asked.text.find("partly migrated") != std::string::npos, asked.text);
	BOOST_TEST(asked.text.find("1 more") != std::string::npos, asked.text);

	const edt::journal_offer warned = edt::decide_journal_offer(unfinished, true);
	BOOST_TEST((warned.what == edt::journal_offer::kind::warn));
	BOOST_TEST(warned.id == unfinished.front());
	BOOST_TEST(warned.text.find("--migrate-rollback 20260101T000000Z-0.1.0") != std::string::npos, warned.text);
}

BOOST_AUTO_TEST_CASE(TheIdIsShownWithoutControlCharacters)
{
	const edt::journal_offer asked = edt::decide_journal_offer({ std::string("a\nb\x1b") }, false);
	BOOST_TEST(asked.title.find('\n') == std::string::npos);
	BOOST_TEST(asked.title.find('\x1b') == std::string::npos);
	// The id itself is kept as it is: the rollback validates it.
	BOOST_TEST(asked.id == std::string("a\nb\x1b"));
}

BOOST_AUTO_TEST_SUITE_END()
