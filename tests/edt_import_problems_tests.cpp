#include <boost/test/unit_test.hpp>
#include "edt_import_problems.h"
#include <string>
#include <vector>

// The Problems rows of a person's import: a texture the model names that was not loaded, and a skipped embedded texture,
// which the importer lists as "*<index>: <why>" (#1206).

BOOST_AUTO_TEST_SUITE(EdtImportProblems)

BOOST_AUTO_TEST_CASE(ASkippedEmbeddedTexture_IsNamedByItsIndexAndReason_AnotherByItsName)
{
	edt::import_intermediate imported;
	imported.missing_textures = { "*3: an embedded texture without data", "rock.png", "*x: not an index", "*12" };
	const std::vector<edt::problem> rows = edt::import_problems(imported, "m.glb");
	BOOST_REQUIRE(rows.size() == 4u);
	BOOST_TEST(rows[0].message == "'m.glb': embedded texture 3 was skipped: an embedded texture without data: its material has no sampler for it");
	BOOST_TEST(rows[1].message == "'m.glb': texture 'rock.png' was not loaded (missing or not readable): its material has no sampler for it");
	BOOST_TEST(rows[2].message.find("texture '*x: not an index' was not loaded") != std::string::npos);
	BOOST_TEST(rows[3].message.find("texture '*12' was not loaded") != std::string::npos);
	for (const edt::problem& row : rows) BOOST_TEST((row.severity == edt::problem_severity::warning));
}

BOOST_AUTO_TEST_SUITE_END()
