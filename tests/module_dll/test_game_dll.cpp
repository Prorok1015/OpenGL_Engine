#include "test_game_dll_api.h"
#include <snk/application/app_module_entry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_component_registry.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/scn_animation.h>
#include <snk/scene/level/scn_prefab_spawn.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_scene_layer.h>

#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

// A game module as its own shared library, the way a game.dll is built (#220 PR-7): it registers into the layer
// module_loader gives it and is loaded by tests/module_dll/app_module_dll_tests.cpp through native_library_source.

namespace dll_game {
	BOOST_DESCRIBE_STRUCT(dll_component, (), (value))
}

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

// The builds a reload swaps between (#435, CMakeLists.txt): TEST_DLL_V2 moves by 2, TEST_DLL_FAILING fails to register,
// TEST_DLL_BAD_API reports the next major API version.
#if defined(TEST_DLL_V2)
constexpr int k_step = 2;
#else
constexpr int k_step = 1;
#endif
#if defined(TEST_DLL_FAILING)
constexpr bool k_fails = true;
#else
constexpr bool k_fails = false;
#endif
#if defined(TEST_DLL_BAD_RESTORE)
constexpr bool k_keeper_traps = true;
#else
constexpr bool k_keeper_traps = false;
#endif

struct dll_keeper : ecs::system_interface
{
	dll_keeper()
	{
		if constexpr (k_keeper_traps) {
			std::error_code ec;
			if (std::filesystem::exists(dll_game::keeper_trap_file, ec)) {
				throw std::runtime_error("the keeper of this build cannot start");
			}
		}
	}
	void run(entt::view<entt::get_t<dll_game::dll_component>> view) { frames += static_cast<int>(view.size()); }
	int frames = 0;
};

void dll_move(entt::view<entt::get_t<dll_game::dll_component>> view)
{
	for (auto&& [entity, component] : view.each()) {
		component.value += k_step;
	}
}

void dll_animate(entt::registry& reg, entt::view<entt::get_t<scn::animation_controller_component>> models)
{
	for (auto&& [model, controller] : models.each()) {
		if (controller.current_animation == "idle") {
			scn::play_animation(reg, model, "walk");
		} else if (controller.current_animation == "walk") {
			scn::play_animation(reg, model, "jump", false);
		}
	}
}

// One call to spawn a segment, one to remove one: no hand-made entities, no cleanup of the systems' queues.
struct dll_snake : ecs::system_interface
{
	void run(entt::registry& reg)
	{
		// Entity handles do not survive a rebuild of the world (reload_context, Play's restore): another registry's are dropped.
		if (owner != &reg) {
			segments.clear();
			owner = &reg;
		}
		eng::transform3d at;
		at.set_pos(glm::vec3(static_cast<float>(spawned++), 0.0f, 0.0f));
		const entt::entity head = scn::spawn_prefab(reg, res::tag{ std::string{ dll_game::segment_tag } }, at);
		if (head == entt::null) return;
		segments.push_back(head);
		if (segments.size() > dll_game::snake_length) {
			scn::despawn(reg, segments.front());
			segments.pop_front();
		}
	}
	std::deque<entt::entity> segments;
	const entt::registry* owner = nullptr;
	int spawned = 0;
};

class test_game_dll_module final : public gs::game_module
{
public:
	std::string_view name() const override { return dll_game::module_name; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		if constexpr (k_fails) {
			static_cast<void>(layer);
			static_cast<void>(data);
			throw std::runtime_error("this build of test_game_dll fails to register");
		} else {
			register_everything(layer, data);
		}
	}

	void shutdown_content(scn::scene_layer&, ds::app_data_storage& data) override
	{
		// Also after a registration that failed before it was made.
		if (data.has_value<dll_service>()) {
			data.destruct<dll_service>();
		}
	}

private:
	static void register_everything(scn::scene_layer& layer, ds::app_data_storage& data)
	{
		layer.descs().register_component_desc<dll_desc>(dll_game::desc_type);
		layer.assembler().register_desc_spawner(dll_game::desc_type, [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<dll_game::dll_component>(e, dll_game::dll_component{ static_cast<const dll_desc&>(base).value });
		});
		layer.systems().register_system<&dll_move>(std::string{ dll_game::system_name });
		layer.systems().register_system<&dll_animate>(std::string{ dll_game::animate_system_name });
		layer.systems().register_system<&dll_keeper::run, dll_keeper>(std::string{ dll_game::keeper_system_name }, {});
		layer.systems().register_system<&dll_snake::run, dll_snake>(std::string{ dll_game::snake_system_name }, {});
		layer.components().add<dll_game::dll_component>(std::string{ dll_game::component_name });
		// Its functions and schema live in this library: the layer drops them before the library goes.
		layer.catalog().add<dll_game::dll_component>(std::string{ dll_game::component_name },
			desc::bind_described<dll_game::dll_component>(dll_game::component_name, 1, desc::schema_kind::component));
		ecs::register_component<dll_game::dll_component>(layer.meta());
		data.require<gs::module_context>().register_component(
			desc::parse_type_schema(boost::json::parse(dll_game::schema_component_json).as_object()));
		auto& service = data.construct<dll_service>();

		// Only when the host stored them: it checks that the callbacks are registered (held_count).
		auto* frames = &service.frames;
		for (const std::string_view name : { dll_game::waited_tag, dll_game::failing_tag }) {
			const res::tag tag{ std::string{ name } };
			if (layer.resources().exists(tag)) {
				layer.resources().require<desc::desc_base>(tag).then(layer.tasks(), [frames](desc::desc_base&) { ++*frames; });
			}
		}
		if (layer.resources().exists(res::tag{ std::string{ dll_game::base_tag } })) {
			for (const std::string_view name : { dll_game::base_tag, dll_game::base_missing_tag }) {
				layer.resources().require<res::text_resource>(res::tag{ std::string{ name } })
					.then(layer.tasks(), [frames](res::text_resource&) { ++*frames; });
			}
		}
		data.require<gs::module_context>().run(ds::lane::compute, ds::task_desc{ .name = "dll.task" }, [frames](ds::task_context&) {})
			.then_main(layer.tasks(), [frames](ds::task_result<void>&) { ++*frames; });
	}
};

}

#if defined(TEST_DLL_BAD_LAYOUT)
// Built as if against an engine whose types have other layouts (#614).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::module_layout_key() ^ 1u)
#endif

#if defined(TEST_DLL_BAD_API)
extern "C" {
SNK_MODULE_EXPORT std::uint32_t snk_module_api_version() { return SNK_MODULE_API_VERSION + 0x0001'0000u; }
SNK_MODULE_EXPORT const struct snk_module_build_info* snk_module_build_info()
{
	static const struct snk_module_build_info info = [] {
		auto built = ::app::modules::make_build_info();
		built.api = SNK_MODULE_API_VERSION + 0x0001'0000u;
		return built;
	}();
	return &info;
}
SNK_MODULE_EXPORT ::app::modules::module_interface* snk_create_module() { return new test_game_dll_module(); }
SNK_MODULE_EXPORT void snk_destroy_module(::app::modules::module_interface* module) { delete module; }
// Another layout too: the host reports the api, which says what to change, not the layout (#614).
SNK_MODULE_EXPORT std::uint64_t snk_module_layout_key() { return ::gs::module_layout_key() ^ 1u; }
}
#else
SNK_DECLARE_MODULE(test_game_dll_module)
#endif
