#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <memory>

// The editor takes the game services at the moment of use (#220, MODULE_LAYER.md): they are the game layer's while
// there is one, the base ones otherwise, and they follow a change of the game layer.

namespace {

struct services_fixture : test_util::scene_env {
	services_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot))
	{
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
};

}

BOOST_FIXTURE_TEST_SUITE(edt_editor_services_tests, services_fixture)

BOOST_AUTO_TEST_CASE(without_a_game_layer_the_base_services_are_used)
{
	BOOST_TEST(services.active_layer() == nullptr);
	BOOST_TEST(services.descs() == descs.get());
	BOOST_TEST(services.resources() == rs.get());
	BOOST_TEST(services.assembler() == assembler.get());
	BOOST_TEST(services.sfactory() == &sf);
}

BOOST_AUTO_TEST_CASE(the_services_follow_the_game_layer)
{
	auto& first = mgr->create_layer();
	mgr->set_game_layer(&first);
	BOOST_TEST(services.active_layer() == &first);
	BOOST_TEST(services.descs() == &first.descs());
	BOOST_TEST(services.resources() == &first.resources());
	BOOST_TEST(services.assembler() == &first.assembler());
	BOOST_TEST(services.sfactory() == &first.systems());

	auto& second = mgr->create_layer();
	mgr->set_game_layer(&second);
	BOOST_TEST(services.descs() == &second.descs());
	BOOST_TEST(services.level_set()->layer() == &second);

	mgr->destroy_layer(first);
	mgr->destroy_layer(second);
	BOOST_TEST(services.active_layer() == nullptr);
	BOOST_TEST(services.descs() == descs.get());
}

BOOST_AUTO_TEST_CASE(without_the_context_manager_nothing_but_the_base_is_left)
{
	mgr.reset();
	BOOST_TEST(services.level_set() == nullptr);
	BOOST_TEST(services.active_layer() == nullptr);
	BOOST_TEST(services.assembler() == assembler.get());
}

BOOST_AUTO_TEST_SUITE_END()
