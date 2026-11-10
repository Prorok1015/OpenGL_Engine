#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "edt_agent_model_guard.h"
#include <snk/command/cmd_registry.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#include <boost/json/parse.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// #681: what counts as an agent naming a model file. The extension is compared the way the resolver would pick an
// adapter, but more strictly (no case, trailing spaces and dots dropped), and the list is what is registered now.

namespace {

struct no_model_adapter : core::res::adapter_interface
{
	std::shared_ptr<res::resource_entry> deserialize(const res::tag&, const std::vector<std::byte>&) const override { return nullptr; }
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

const std::vector<std::string> k_models{ "glb", "obj", "fbx", "gltf" };

cmd::command_desc command_named(std::string id)
{
	cmd::command_desc desc;
	desc.id = std::move(id);
	return desc;
}

cmd::call agent_call(std::string_view args)
{
	cmd::call request;
	request.args = boost::json::parse(args);
	request.from = cmd::origin::agent("s1");
	return request;
}

}

BOOST_AUTO_TEST_SUITE(EdtAgentModelGuard)

BOOST_AUTO_TEST_CASE(TheExtension_IsTakenAsTheResolverWould_WithoutCaseAndTrailingSpacesOrDots)
{
	BOOST_TEST(edt::model_check_extension("res://models/x.glb") == "glb");
	BOOST_TEST(edt::model_check_extension("res://models/X.FBX") == "fbx");
	BOOST_TEST(edt::model_check_extension("res://models/x.Gltf. . ") == "gltf");
	BOOST_TEST(edt::model_check_extension("models\\robot.obj") == "obj");
	BOOST_TEST(edt::model_check_extension("x.glb") == "glb");
	BOOST_TEST(edt::model_check_extension("res://models/x.glb.desc") == "desc");
	BOOST_TEST(edt::model_check_extension("res://models.glb/x") == "");
	BOOST_TEST(edt::model_check_extension("res://models/glb") == "");
	BOOST_TEST(edt::model_check_extension("") == "");
	// res::tag turns every `\` into `/` before it reads the protocol: `res\models\x.glb` is res://models/x.glb (#680 review).
	BOOST_TEST(edt::model_check_extension("res\\models\\x.glb") == "glb");
	BOOST_TEST(edt::model_check_extension("res:\\models\\x.fbx") == "fbx");
	BOOST_TEST(edt::model_check_extension("res\\models\\x.glb\\") == "");
	for (const char* text : { "res\\models\\x.glb", "res:\\m\\x.fbx", "res://m/x.gltf", "m\\x.obj", "res\\a.glb\\b.desc", "x.glb" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::model_check_extension(text) == std::string(res::tag{ text }.extension()));
		}
	}
	// The resolver decodes no percent escapes: "%2Eglb" is a name without that extension, as for the resolver.
	BOOST_TEST(edt::model_check_extension("res://models/x%2Eglb") == "");
}

BOOST_AUTO_TEST_CASE(AModelIsFoundAtAnyDepth_WithItsPointer_ButNotInKeysOrOtherFiles)
{
	const auto found = edt::find_named_model(boost::json::parse(R"json({"a":[1,{"b/c":{"m":"res://m/Robot.GLB "}}]})json"), k_models);
	BOOST_REQUIRE(found);
	BOOST_TEST(found->pointer == "/a/1/b~1c/m");
	BOOST_TEST(found->value == "res://m/Robot.GLB ");
	BOOST_TEST(found->extension == "glb");

	BOOST_TEST(!edt::find_named_model(boost::json::parse(R"json({"x.glb":"res://levels/a.desc","n":"robot"})json"), k_models));
	BOOST_TEST(!edt::find_named_model(boost::json::parse(R"json({"m":"res://m/x.glb"})json"), {}));
}

BOOST_AUTO_TEST_CASE(TheCheck_FollowsTheRegisteredModelAdapter_AndLetsImportModelThrough)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	res::resource_system resources(tasks);
	const auto check = edt::make_agent_model_check(resources);
	const cmd::call names_a_model = agent_call(R"json({"fields":{"material":"res://models/agent_named.GLB"}})json");

	// No model adapter (a build without the importer): nothing would parse it, nothing is refused.
	BOOST_TEST(!check(command_named("component.add"), names_a_model));

	resources.registrate_adapter<no_model_adapter>(scn::model_importer_adapter::INFO);
	BOOST_TEST(resources.registered_extensions(scn::model_importer_adapter::INFO).size() == scn::model_importer_adapter::INFO.extensions.size());
	const std::optional<cmd::result> refused = check(command_named("component.add"), names_a_model);
	BOOST_REQUIRE(refused);
	BOOST_TEST(refused->code == cmd::codes::denied);
	BOOST_TEST(refused->error.find("/fields/material") != std::string::npos);
	BOOST_TEST(refused->error.find("file.import_model") != std::string::npos);
	BOOST_TEST(check(command_named("component.add"), agent_call(R"json({"fields":{"material":"res\\models\\agent_named.glb"}})json")).has_value());
	BOOST_TEST(!check(command_named("file.import_model"), agent_call(R"json({"path":"/home/me/robot.glb"})json")));
	BOOST_TEST(!check(command_named("component.add"), agent_call(R"json({"fields":{"material":"res://materials/a.desc"}})json")));

	resources.unregistrate_adapter(scn::model_importer_adapter::INFO);
	BOOST_TEST(resources.registered_extensions(scn::model_importer_adapter::INFO).empty());
	BOOST_TEST(!check(command_named("component.add"), names_a_model));
}

BOOST_AUTO_TEST_SUITE_END()
