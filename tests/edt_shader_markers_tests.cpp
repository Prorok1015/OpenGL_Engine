#include <boost/test/unit_test.hpp>
#include "edt_shader_markers.h"

namespace {

using namespace edt;

sourced_problem row(std::string source, std::string file, int line, std::string message, std::string code = "shader/compile", int column = 0,
	problem_severity severity = problem_severity::error)
{
	sourced_problem out;
	out.source = std::move(source);
	out.item.file = std::move(file);
	out.item.line = line;
	out.item.column = column;
	out.item.code = std::move(code);
	out.item.message = std::move(message);
	out.item.severity = severity;
	return out;
}

constexpr const char* k_file = "res://shaders/scene.frag";

}

BOOST_AUTO_TEST_SUITE(EdtShaderMarkers)

BOOST_AUTO_TEST_CASE(OnlyTheRowsOfTheShaderSource_ForThisFile_AreTaken)
{
	const std::vector<sourced_problem> rows{ row("shader", k_file, 3, "mine"), row("shader", "res://shaders/other.frag", 3, "another file"),
		row("build", k_file, 3, "not a shader row") };

	const shader_marker_set set = shader_markers_for(rows, k_file, 10);

	BOOST_REQUIRE(set.markers.size() == 1u);
	BOOST_TEST(set.markers[0].line == 3);
	BOOST_TEST(set.markers[0].text.find("mine") != std::string::npos);
	BOOST_TEST(set.unplaced.empty());
}

BOOST_AUTO_TEST_CASE(TwoRowsOnOneLine_BecomeOneMarker_WithTheWorseSeverity)
{
	const std::vector<sourced_problem> rows{ row("shader", k_file, 5, "first", "shader/compile", 0, problem_severity::warning),
		row("shader", k_file, 5, "second", "shader/compile", 0, problem_severity::error) };

	const shader_marker_set set = shader_markers_for(rows, k_file, 10);

	BOOST_REQUIRE(set.markers.size() == 1u);
	BOOST_TEST((set.markers[0].severity == problem_severity::error));
	BOOST_TEST(set.markers[0].text == "[shader/compile] first\n[shader/compile] second");
}

BOOST_AUTO_TEST_CASE(MarkersComeInLineOrder)
{
	const std::vector<sourced_problem> rows{ row("shader", k_file, 9, "late"), row("shader", k_file, 2, "early") };

	const shader_marker_set set = shader_markers_for(rows, k_file, 10);

	BOOST_REQUIRE(set.markers.size() == 2u);
	BOOST_TEST(set.markers[0].line == 2);
	BOOST_TEST(set.markers[1].line == 9);
}

BOOST_AUTO_TEST_CASE(ARowWithoutALine_IsUnplaced_AndAColumnIsInItsText)
{
	const std::vector<sourced_problem> rows{ row("shader", k_file, 0, "link failed", "shader/link"), row("shader", k_file, 4, "bad token", "shader/compile", 12) };

	const shader_marker_set set = shader_markers_for(rows, k_file, 10);

	BOOST_REQUIRE(set.unplaced.size() == 1u);
	BOOST_TEST(set.unplaced[0] == "[shader/link] link failed");
	BOOST_REQUIRE(set.markers.size() == 1u);
	BOOST_TEST(set.markers[0].text == "[shader/compile] bad token (column 12)");
}

BOOST_AUTO_TEST_CASE(ARowPastTheLastLine_GoesOnTheLastLine_AndNoLinesMeansEveryRowIsUnplaced)
{
	const std::vector<sourced_problem> rows{ row("shader", k_file, 99, "the text was shortened") };

	const shader_marker_set clamped = shader_markers_for(rows, k_file, 7);
	BOOST_REQUIRE(clamped.markers.size() == 1u);
	BOOST_TEST(clamped.markers[0].line == 7);

	const shader_marker_set none = shader_markers_for(rows, k_file, 0);
	BOOST_TEST(none.markers.empty());
	BOOST_TEST(none.unplaced.size() == 1u);
}

BOOST_AUTO_TEST_CASE(PastTheLimit_TheRestIsCounted_NotMarked)
{
	std::vector<sourced_problem> rows;
	for (int line = 1; line <= static_cast<int>(k_max_shader_markers) + 5; ++line) {
		rows.push_back(row("shader", k_file, line, "x"));
	}

	const shader_marker_set set = shader_markers_for(rows, k_file, 1000);

	BOOST_TEST(set.markers.size() == k_max_shader_markers);
	BOOST_TEST(set.omitted == 5u);
}

BOOST_AUTO_TEST_CASE(ALongMessage_IsClipped)
{
	const std::vector<sourced_problem> rows{ row("shader", k_file, 1, std::string(5000, 'a')) };

	const shader_marker_set set = shader_markers_for(rows, k_file, 3);

	BOOST_REQUIRE(set.markers.size() == 1u);
	BOOST_TEST(set.markers[0].text.size() <= k_max_shader_marker_text + 3u);
}

BOOST_AUTO_TEST_SUITE_END()
