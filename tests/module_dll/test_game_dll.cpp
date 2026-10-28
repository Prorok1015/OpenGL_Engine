#include "test_game_dll_api.h"
#include <snk/application/app_module_entry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_component_registry.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_scene_layer.h>

#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A game module as its own shared library, the way a game.dll is built (#220 PR-7): it registers into the layer
// module_loader gives it and is loaded by tests/module_dll/app_module_dll_tests.cpp through native_library_source.

namespace {

struct dll_service {
	int frames = 0;
};

class dll_desc final : public desc::desc_base
{
public:
	int value = 0;

	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (const auto* v = obj.if_contains("value")) {
			value = static_cast<int>(v->to_number<std::int64_t>());
		}
	}

	void serialize(boost::json::object& obj) const override { obj["value"] = value; }
};

void dll_move(entt::view<entt::get_t<dll_game::dll_component>> view)
{
	for (auto&& [entity, component] : view.each()) {
		++component.value;
	}
}

class test_game_dll_module final : public gs::game_module
{
public:
	std::string_view name() const override { return dll_game::module_name; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		layer.descs().register_component_desc<dll_desc>(dll_game::desc_type);
		layer.assembler().register_desc_spawner(dll_game::desc_type, [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<dll_game::dll_component>(e, dll_game::dll_component{ static_cast<const dll_desc&>(base).value });
		});
		layer.systems().register_system<&dll_move>(std::string{ dll_game::system_name });
		layer.components().add<dll_game::dll_component>(std::string{ dll_game::component_name });
		ecs::register_component<dll_game::dll_component>(layer.meta());
		auto& service = data.construct<dll_service>();

		// Only when the host stored them: it checks that the callbacks are registered (held_count).
		auto* frames = &service.frames;
		for (const std::string_view name : { dll_game::waited_tag, dll_game::failing_tag }) {
			const res::tag tag{ std::string{ name } };
			if (layer.resources().exists(tag)) {
				layer.resources().require<desc::desc_base>(tag).then(layer.tasks(), [frames](desc::desc_base&) { ++*frames; });
			}
		}
		data.require<gs::module_context>().run(ds::lane::compute, ds::task_desc{ .name = "dll.task" }, [frames](ds::task_context&) {})
			.then_main(layer.tasks(), [frames](ds::task_result<void>&) { ++*frames; });
	}

	void shutdown_content(scn::scene_layer&, ds::app_data_storage& data) override
	{
		data.destruct<dll_service>();
	}
};

}

SNK_DECLARE_MODULE(test_game_dll_module)
