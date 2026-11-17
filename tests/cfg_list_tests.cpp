#include <boost/test/unit_test.hpp>
#include <snk/config/cfg_api.h>
#include <snk/config/cfg_variables.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// List values of the configuration (#411): yaml sequences, --set, config_view. Everything goes through
// cfg::load_config_text / cfg::set_variable, so none of it needs the once-per-process initialize_configs.

namespace {

using strings = std::vector<std::string>;

bool load(const char* yaml)
{
	return cfg::load_config_text(yaml, std::filesystem::path{});
}

}

BOOST_AUTO_TEST_SUITE(CfgListTests)

BOOST_AUTO_TEST_CASE(Yaml_BlockSequence_FillsTheListInOrder)
{
	cfg::variable<strings> v{ "cfglist/block", strings{ "default" } };

	BOOST_TEST(load("cfglist:\n  block:\n    - first\n    - second\n    - third\n"));

	BOOST_TEST((*v == strings{ "first", "second", "third" }));
}

BOOST_AUTO_TEST_CASE(Yaml_FlowSequence_FillsTheListAndKeepsNumbersAsText)
{
	cfg::variable<strings> v{ "cfglist/flow", strings{} };

	BOOST_TEST(load("cfglist:\n  flow: [edt://settings/**, 12, true, \"*.ini\"]\n"));

	BOOST_TEST((*v == strings{ "edt://settings/**", "12", "true", "*.ini" }));
}

BOOST_AUTO_TEST_CASE(Yaml_EmptySequence_EmptiesTheListButAMissingKeyKeepsTheDefault)
{
	cfg::variable<strings> emptied{ "cfglist/emptied", strings{ "default" } };
	cfg::variable<strings> kept{ "cfglist/kept", strings{ "default" } };

	BOOST_TEST(load("cfglist:\n  emptied: []\n"));

	BOOST_TEST(emptied->empty());
	BOOST_TEST((*kept == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(Yaml_SecondLoad_ReplacesTheListInsteadOfMergingIt)
{
	cfg::variable<strings> v{ "cfglist/replace", strings{} };
	load("cfglist:\n  replace: [a, b]\n");

	load("cfglist:\n  replace: [c]\n");

	BOOST_TEST((*v == strings{ "c" }));
}

BOOST_AUTO_TEST_CASE(Yaml_ScalarWhereAListIsExpected_IsIgnored)
{
	cfg::variable<strings> v{ "cfglist/scalar", strings{ "default" } };

	BOOST_TEST(load("cfglist:\n  scalar: edt://settings/**\n"));

	BOOST_TEST((*v == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(Yaml_ListWhereAScalarIsExpected_IsIgnored)
{
	cfg::variable<std::string> text{ "cfglist/text", "default" };
	cfg::variable<int> number{ "cfglist/number", 7 };

	BOOST_TEST(load("cfglist:\n  text: [a, b]\n  number: [1]\n"));

	BOOST_TEST(*text == "default");
	BOOST_TEST(*number == 7);
}

BOOST_AUTO_TEST_CASE(Yaml_NestedListOrMapInsideAList_IsIgnored)
{
	cfg::variable<strings> nested{ "cfglist/nested", strings{ "default" } };
	cfg::variable<strings> mapped{ "cfglist/mapped", strings{ "default" } };

	BOOST_TEST(load("cfglist:\n  nested: [a, [b]]\n  mapped: [a, {k: v}]\n"));

	BOOST_TEST((*nested == strings{ "default" }));
	BOOST_TEST((*mapped == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(Yaml_InvalidText_ReturnsFalseAndChangesNothing)
{
	cfg::variable<strings> v{ "cfglist/invalid", strings{ "default" } };

	BOOST_TEST(!load("cfglist:\n  invalid: [a, b"));

	BOOST_TEST((*v == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_FlowSequence_ReplacesTheList)
{
	cfg::variable<strings> v{ "cfglist/set_flow", strings{ "default" } };

	cfg::set_variable("cfglist/set_flow", "[a, b]");
	BOOST_TEST((*v == strings{ "a", "b" }));

	cfg::set_variable("cfglist/set_flow", "[c]");
	BOOST_TEST((*v == strings{ "c" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_EmptyFlowSequence_EmptiesTheList)
{
	cfg::variable<strings> v{ "cfglist/set_empty", strings{ "default" } };

	cfg::set_variable("cfglist/set_empty", "[]");

	BOOST_TEST(v->empty());
}

BOOST_AUTO_TEST_CASE(SetVariable_ValueWithoutABracket_IsAListOfOneElement)
{
	cfg::variable<strings> v{ "cfglist/set_one", strings{ "default" } };

	cfg::set_variable("cfglist/set_one", "edt://settings/**");

	BOOST_TEST((*v == strings{ "edt://settings/**" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_QuotedElementsMayStartWithAStar)
{
	cfg::variable<strings> v{ "cfglist/set_star", strings{} };

	cfg::set_variable("cfglist/set_star", "[\"*.ini\", \"a,b\"]");

	BOOST_TEST((*v == strings{ "*.ini", "a,b" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_BrokenOrNestedFlowSequence_KeepsTheList)
{
	cfg::variable<strings> v{ "cfglist/set_bad", strings{ "default" } };

	cfg::set_variable("cfglist/set_bad", "[a, [b]]");
	BOOST_TEST((*v == strings{ "default" }));
	cfg::set_variable("cfglist/set_bad", "[a, b");
	BOOST_TEST((*v == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_ScalarVariable_ParsesTheTextAsBefore)
{
	cfg::variable<std::string> text{ "cfglist/set_scalar_text", "default" };
	cfg::variable<int> number{ "cfglist/set_scalar_number", 1 };

	cfg::set_variable("cfglist/set_scalar_text", "[a]");
	cfg::set_variable("cfglist/set_scalar_number", "42");

	BOOST_TEST(*text == "[a]");
	BOOST_TEST(*number == 42);
}

BOOST_AUTO_TEST_CASE(ConfigView_List_ReturnsTheElementsAndValueTheJsonArray)
{
	cfg::variable<strings> v{ "cfglist/view", strings{ "a", "b\"c", "d\\e" } };
	cfg::variable<std::string> scalar{ "cfglist/view_scalar", "text" };
	const cfg::config_view view;

	BOOST_TEST((view.list("cfglist/view") == std::optional<strings>{ strings{ "a", "b\"c", "d\\e" } }));
	BOOST_TEST(view.value("cfglist/view").value_or("") == "[\"a\",\"b\\\"c\",\"d\\\\e\"]");
	BOOST_TEST(!view.list("cfglist/view_scalar").has_value());
	BOOST_TEST(view.value("cfglist/view_scalar").value_or("") == "text");
	BOOST_TEST(!view.list("cfglist/unknown").has_value());
}

BOOST_AUTO_TEST_CASE(ConfigView_EmptyList_IsAnEmptyJsonArray)
{
	cfg::variable<strings> v{ "cfglist/view_empty", strings{} };
	const cfg::config_view view;

	BOOST_TEST(view.value("cfglist/view_empty").value_or("") == "[]");
	BOOST_TEST((view.list("cfglist/view_empty") == std::optional<strings>{ strings{} }));
}

BOOST_AUTO_TEST_CASE(Registration_VariablesOfOneName_AllTakeTheListAndLeaveOneByOne)
{
	auto first = std::make_unique<cfg::variable<strings>>("cfglist/shared", strings{});
	cfg::variable<strings> second{ "cfglist/shared", strings{} };

	load("cfglist:\n  shared: [x, y]\n");
	BOOST_TEST((**first == strings{ "x", "y" }));
	BOOST_TEST((*second == strings{ "x", "y" }));

	first.reset();
	load("cfglist:\n  shared: [z]\n");
	BOOST_TEST((*second == strings{ "z" }));
}

BOOST_AUTO_TEST_CASE(Registration_ListAndScalarOfOneName_EachTakesItsOwnKindOnly)
{
	cfg::variable<strings> list{ "cfglist/mixed", strings{ "default" } };
	cfg::variable<std::string> text{ "cfglist/mixed", "default" };

	load("cfglist:\n  mixed: [a]\n");
	BOOST_TEST((*list == strings{ "a" }));
	BOOST_TEST(*text == "default");

	load("cfglist:\n  mixed: scalar\n");
	BOOST_TEST((*list == strings{ "a" }));
	BOOST_TEST(*text == "scalar");
}

BOOST_AUTO_TEST_CASE(Extends_ChildListReplacesTheBaseListAsAWhole)
{
	test_util::temp_dir dir{ "cfglist_extends" };
	dir.write("base.yaml", "cfglist:\n  extended: [a, b, c]\n  inherited: [x, y]\n  scalar: from_base\n");
	dir.write("conf/child.yaml", "extends:\n  - ../base.yaml\ncfglist:\n  extended: [z]\n");
	cfg::variable<strings> extended{ "cfglist/extended", strings{ "default" } };
	cfg::variable<strings> inherited{ "cfglist/inherited", strings{ "default" } };
	cfg::variable<std::string> scalar{ "cfglist/scalar", "default" };

	BOOST_TEST(cfg::load_config_file(dir.path() / "conf" / "child.yaml"));

	BOOST_TEST((*extended == strings{ "z" }));
	BOOST_TEST((*inherited == strings{ "x", "y" }));
	BOOST_TEST(*scalar == "from_base");
}

BOOST_AUTO_TEST_CASE(LoadConfigFile_MissingFileOrBrokenBase_ReturnsFalse)
{
	test_util::temp_dir dir{ "cfglist_missing" };
	dir.write("child.yaml", "extends:\n  - no_such_base.yaml\n");

	BOOST_TEST(!cfg::load_config_file(dir.path() / "no_such.yaml"));
	BOOST_TEST(!cfg::load_config_file(dir.path() / "child.yaml"));
}

BOOST_AUTO_TEST_CASE(Yaml_NullElementInAList_IsRefusedLikeANestedOne)
{
	cfg::variable<strings> flow{ "cfglist/null_flow", strings{ "default" } };
	cfg::variable<strings> block{ "cfglist/null_block", strings{ "default" } };
	cfg::variable<strings> word{ "cfglist/null_word", strings{ "default" } };

	BOOST_TEST(load("cfglist:\n  null_flow: [a, ~]\n  null_block:\n    - a\n    - ~\n  null_word: [a, null]\n"));

	BOOST_TEST((*flow == strings{ "default" }));
	BOOST_TEST((*block == strings{ "default" }));
	BOOST_TEST((*word == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(Yaml_QuotedNullWordsInAList_AreTextNotNull)
{
	cfg::variable<strings> v{ "cfglist/quoted_null", strings{} };

	BOOST_TEST(load("cfglist:\n  quoted_null: [\"~\", \"null\"]\n"));

	BOOST_TEST((*v == strings{ "~", "null" }));
}

BOOST_AUTO_TEST_CASE(Yaml_NullValue_IsIgnoredForListsAndScalarsAlike)
{
	cfg::variable<strings> list{ "cfglist/null_list", strings{ "default" } };
	cfg::variable<std::string> text{ "cfglist/null_text", "default" };
	cfg::variable<int> number{ "cfglist/null_number", 7 };

	BOOST_TEST(load("cfglist:\n  null_list: ~\n  null_text:\n  null_number: ~\n"));

	BOOST_TEST((*list == strings{ "default" }));
	BOOST_TEST(*text == "default");
	BOOST_TEST(*number == 7);
}

BOOST_AUTO_TEST_CASE(SetVariable_NullElementInAFlowSequence_KeepsTheList)
{
	cfg::variable<strings> v{ "cfglist/set_null", strings{ "default" } };

	cfg::set_variable("cfglist/set_null", "[a, ~]");

	BOOST_TEST((*v == strings{ "default" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_AnUnquotedCommaSplitsAndABackslashDoesNotEscapeIt)
{
	cfg::variable<strings> split{ "cfglist/set_comma", strings{} };
	cfg::variable<strings> quoted{ "cfglist/set_comma_quoted", strings{} };

	cfg::set_variable("cfglist/set_comma", "[a\\, b]");
	cfg::set_variable("cfglist/set_comma_quoted", "[\"a, b\", c]");

	BOOST_TEST((*split == strings{ "a\\", "b" }));
	BOOST_TEST((*quoted == strings{ "a, b", "c" }));
}

BOOST_AUTO_TEST_SUITE_END()
