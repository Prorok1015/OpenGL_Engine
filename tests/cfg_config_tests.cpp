#include <boost/test/unit_test.hpp>
#include <snk/config/cfg_variables.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <string>
#include <vector>

// NOTE: cfg::initialize_configs(path) is guarded by a process-wide "already initialized" flag,
// so only ONE test in the whole binary may load a config file. ConfigFile_* below is that test;
// everything else goes through variable::update() / --set which have no such restriction.

namespace {

int run_cli(std::vector<std::string> args)
{
	std::vector<char*> argv;
	static std::string program = "unit_tests";
	argv.push_back(program.data());
	for (auto& a : args) argv.push_back(a.data());
	return cfg::initialize_configs(static_cast<int>(argv.size()), argv.data()) ? 1 : 0;
}

}

BOOST_AUTO_TEST_SUITE(CfgConfigTests)

BOOST_AUTO_TEST_CASE(Cli_AllowEngineVersionMismatchSetsItsVariable)
{
	// Stands in for core_module's variable of the same name for the length of the test.
	cfg::variable<bool> allow{ "project/allow_engine_version_mismatch", false };
	run_cli({});
	BOOST_TEST(!static_cast<bool>(allow));
	run_cli({ "--allow-engine-version-mismatch" });
	BOOST_TEST(static_cast<bool>(allow));
}

BOOST_AUTO_TEST_CASE(Cli_NonInteractiveSetsItsVariableInBothForms)
{
	for (const std::string& flag : { std::string{ "--non-interactive" }, std::string{ "-n" } }) {
		BOOST_TEST_INFO("flag: " << flag);
		// Stands in for core_module's variable of the same name for the length of the test.
		cfg::variable<bool> non_interactive{ "app/non_interactive", false };
		run_cli({});
		BOOST_TEST(!static_cast<bool>(non_interactive));
		run_cli({ flag });
		BOOST_TEST(static_cast<bool>(non_interactive));
	}
}

BOOST_AUTO_TEST_CASE(Cli_NonInteractiveAfterAMultitokenSetIsStillAFlag)
{
	// Stand in for core_module's variable and some other variable for the length of the test.
	cfg::variable<bool> non_interactive{ "app/non_interactive", false };
	cfg::variable<std::string> other{ "test/cli_other", "" };
	run_cli({ "-s", "test/cli_other", "value", "-n" });
	BOOST_TEST(static_cast<bool>(non_interactive));
	const std::string got = other;
	BOOST_TEST(got == "value");
}

BOOST_AUTO_TEST_CASE(Cli_ProjectIsSetProjectFileAndSetStillWins)
{
	// Stands in for core_module's variable of the same name for the length of the test.
	cfg::variable<std::filesystem::path> project_file{ "project/file", "" };

	run_cli({ "--project", "games/sandbox" });
	const std::filesystem::path via_project = project_file;
	run_cli({ "--set", "project/file", "games/sandbox" });
	const std::filesystem::path via_set = project_file;
	BOOST_TEST(via_project == via_set);
	BOOST_TEST(via_project == std::filesystem::current_path() / "games/sandbox");

	run_cli({ "--project", "a", "--set", "project/file", "b" });
	BOOST_TEST(static_cast<std::filesystem::path>(project_file) == std::filesystem::current_path() / "b");
}

// ---- variable<T>::update parsing ----

BOOST_AUTO_TEST_CASE(Variable_IntParsesDecimalAndNegative)
{
	cfg::variable<int> v{ "cfgtest/int_parse", 7 };
	BOOST_TEST(static_cast<int>(v) == 7);
	v.update({}, "42");
	BOOST_TEST(static_cast<int>(v) == 42);
	v.update({}, "-5");
	BOOST_TEST(static_cast<int>(v) == -5);
}

BOOST_AUTO_TEST_CASE(Variable_IntRejectsGarbageWithException)
{
	cfg::variable<int> v{ "cfgtest/int_bad", 3 };
	BOOST_CHECK_THROW(v.update({}, "abc"), std::invalid_argument);
	BOOST_TEST(static_cast<int>(v) == 3); // value unchanged after a failed parse
}

BOOST_AUTO_TEST_CASE(Variable_FloatParses)
{
	cfg::variable<float> v{ "cfgtest/float", 1.0f };
	v.update({}, "2.5");
	BOOST_TEST(static_cast<float>(v) == 2.5f);
}

BOOST_AUTO_TEST_CASE(Variable_BoolOnlyTrueAnd1AreTrue)
{
	cfg::variable<bool> v{ "cfgtest/bool", false };
	v.update({}, "true");
	BOOST_TEST(static_cast<bool>(v));
	v.update({}, "false");
	BOOST_TEST(!static_cast<bool>(v));
	v.update({}, "1");
	BOOST_TEST(static_cast<bool>(v));
	v.update({}, "0");
	BOOST_TEST(!static_cast<bool>(v));
	// Pins current behavior: anything else (including "yes"/"True") is false.
	v.update({}, "yes");
	BOOST_TEST(!static_cast<bool>(v));
	v.update({}, "True");
	BOOST_TEST(!static_cast<bool>(v));
}

BOOST_AUTO_TEST_CASE(Variable_StringKeepsValueVerbatim)
{
	cfg::variable<std::string> v{ "cfgtest/string", "def" };
	v.update({}, "hello world/with spaces");
	BOOST_TEST(*v == "hello world/with spaces");
	BOOST_TEST(v->size() == std::string("hello world/with spaces").size());
}

BOOST_AUTO_TEST_CASE(Variable_PathIsResolvedRelativeToConfigLocation)
{
	cfg::variable<std::filesystem::path> v{ "cfgtest/path", "default" };
	v.update(std::filesystem::path("/etc/app"), "data/assets");
	BOOST_TEST((*v).generic_string() == "/etc/app/data/assets");
}

BOOST_AUTO_TEST_CASE(Variable_DestructionUnregistersFromCli)
{
	{
		cfg::variable<int> v{ "cfgtest/scoped", 1 };
		run_cli({ "--set", "cfgtest/scoped", "9" });
		BOOST_TEST(static_cast<int>(v) == 9);
	}
	// Variable is gone: setting it must be a harmless no-op (no dangling pointer call).
	BOOST_CHECK_NO_THROW(run_cli({ "--set", "cfgtest/scoped", "10" }));
}

// ---- variable_map ----

BOOST_AUTO_TEST_CASE(VariableMap_StoresEntriesBySuffix)
{
	cfg::variable_map m{ "cfgtest/map" };
	BOOST_TEST(m.entries().empty());
	m.update_entry("edt", "debug");
	m.update_entry("a/b", "x");
	m.update_entry("edt", "trace"); // overwrite
	BOOST_TEST(m.entries().size() == 2u);
	BOOST_TEST(m.entries().at("edt") == "trace");
	BOOST_TEST(m.entries().at("a/b") == "x");
}

// ---- command line ----

BOOST_AUTO_TEST_CASE(Cli_SetOverridesMultiplePairs)
{
	cfg::variable<int> a{ "cfgtest/cli_a", 1 };
	cfg::variable<std::string> b{ "cfgtest/cli_b", "x" };
	cfg::variable<bool> c{ "cfgtest/cli_c", false };

	BOOST_TEST(run_cli({ "--set", "cfgtest/cli_a", "5", "cfgtest/cli_b", "hello", "cfgtest/cli_c", "true" }) == 1);
	BOOST_TEST(static_cast<int>(a) == 5);
	BOOST_TEST(*b == "hello");
	BOOST_TEST(static_cast<bool>(c));
}

BOOST_AUTO_TEST_CASE(Cli_SetUnknownVariableIsIgnored)
{
	cfg::variable<int> a{ "cfgtest/cli_known", 1 };
	BOOST_TEST(run_cli({ "--set", "cfgtest/does_not_exist", "5", "cfgtest/cli_known", "2" }) == 1);
	BOOST_TEST(static_cast<int>(a) == 2);
}

BOOST_AUTO_TEST_CASE(Cli_PathSetResolvesAgainstWorkingDirectory)
{
	cfg::variable<std::filesystem::path> p{ "cfgtest/cli_path", "def" };
	run_cli({ "--set", "cfgtest/cli_path", "rel/dir" });
	BOOST_TEST((*p) == std::filesystem::current_path() / "rel/dir");
}

BOOST_AUTO_TEST_CASE(Cli_HelpReturnsFalseAndSkipsOverrides)
{
	cfg::variable<int> a{ "cfgtest/help_var", 1 };
	// "help" short-circuits: caller should exit instead of starting the app.
	BOOST_TEST(run_cli({ "--help", "--set", "cfgtest/help_var", "99" }) == 0);
	BOOST_TEST(static_cast<int>(a) == 1);
}

// ---- config file: extends chain + prefix maps + path vars (runs ONCE per process) ----

// ---- variables of the same name, config_view (#405) ----

BOOST_AUTO_TEST_CASE(Variables_OfTheSameNameAreAllSetAndLeaveOnTheirOwn)
{
	cfg::variable<int> first{ "cfgtest/shared", 1 };
	{
		cfg::variable<int> second{ "cfgtest/shared", 2 };
		run_cli({ "--set", "cfgtest/shared", "5" });
		BOOST_TEST(static_cast<int>(first) == 5);
		BOOST_TEST(static_cast<int>(second) == 5);
	}
	// The second one leaving does not take the first one's registration with it.
	run_cli({ "--set", "cfgtest/shared", "6" });
	BOOST_TEST(static_cast<int>(first) == 6);
	BOOST_TEST(cfg::config_view{}.value("cfgtest/shared").value_or("") == "6");
}

BOOST_AUTO_TEST_CASE(ConfigView_ReadsRegisteredVariablesAsText)
{
	const cfg::config_view view;
	cfg::variable<bool> flag{ "cfgtest/view_bool", true };
	cfg::variable<std::string> text{ "cfgtest/view_string", "abc" };
	BOOST_TEST(view.value("cfgtest/view_bool").value_or("") == "true");
	BOOST_TEST(view.value("cfgtest/view_string").value_or("") == "abc");
	BOOST_TEST(!view.has("cfgtest/view_missing"));
}

BOOST_AUTO_TEST_CASE(ConfigFile_ExtendsChainOverridesAndPathsRelativeToDefiningFile)
{
	test_util::temp_dir dir("cfg_files");
	dir.write("base/base.yaml",
		"common:\n"
		"  width: 100\n"
		"  height: 200\n"
		"  name: base_name\n"
		"  data_dir: base_data\n"
		"nested:\n"
		"  deep:\n"
		"    flag: true\n"
		"logger:\n"
		"  file:\n"
		"    edt: edt.log\n"
		"    a/b: nested_prefix.log\n");
	dir.write("mid/mid.yaml",
		"extends:\n"
		"  - ../base/base.yaml\n"
		"common:\n"
		"  height: 250\n"
		"  mid_dir: mid_data\n");
	auto top = dir.write("top/top.yaml",
		"extends:\n"
		"  - ../mid/mid.yaml\n"
		"common:\n"
		"  name: top_name\n"
		"  top_dir: top_data\n"
		"  unknown_key: ignored\n"
		"logger:\n"
		"  file:\n"
		"    eng: eng.log\n");

	cfg::variable<int> width{ "common/width", -1 };
	cfg::variable<int> height{ "common/height", -1 };
	cfg::variable<std::string> name{ "common/name", "unset" };
	cfg::variable<std::filesystem::path> data_dir{ "common/data_dir", "unset" };
	cfg::variable<std::filesystem::path> mid_dir{ "common/mid_dir", "unset" };
	cfg::variable<std::filesystem::path> top_dir{ "common/top_dir", "unset" };
	cfg::variable<bool> flag{ "nested/deep/flag", false };
	cfg::variable<int> missing{ "common/not_in_any_file", 77 };
	cfg::variable_map files{ "logger/file" };

	cfg::initialize_configs(top);

	// base value inherited through two levels of extends
	BOOST_TEST(static_cast<int>(width) == 100);
	// mid overrides base; child never mentions it
	BOOST_TEST(static_cast<int>(height) == 250);
	// top overrides base
	BOOST_TEST(*name == "top_name");
	// nested maps are addressed with '/'
	BOOST_TEST(static_cast<bool>(flag));
	// variables absent from every file keep their default
	BOOST_TEST(static_cast<int>(missing) == 77);

	// path variables are relative to the directory of the file that DEFINES them
	BOOST_TEST((*data_dir).lexically_normal() == (dir.path() / "base" / "base_data").lexically_normal());
	BOOST_TEST((*mid_dir).lexically_normal() == (dir.path() / "mid" / "mid_data").lexically_normal());
	BOOST_TEST((*top_dir).lexically_normal() == (dir.path() / "top" / "top_data").lexically_normal());

	// prefix maps collect every sub-key (suffix keeps its '/' separators), merged across the chain
	BOOST_TEST(files.entries().size() == 3u);
	BOOST_TEST(files.entries().at("edt") == "edt.log");
	BOOST_TEST(files.entries().at("a/b") == "nested_prefix.log");
	BOOST_TEST(files.entries().at("eng") == "eng.log");

	// A second call is a no-op (guarded): a different file must not be loaded.
	auto other = dir.write("other.yaml", "common:\n  width: 1\n");
	cfg::initialize_configs(other);
	BOOST_TEST(static_cast<int>(width) == 100);

	// --set still wins after the file is loaded
	run_cli({ "--set", "common/width", "640" });
	BOOST_TEST(static_cast<int>(width) == 640);
}

BOOST_AUTO_TEST_SUITE_END()
