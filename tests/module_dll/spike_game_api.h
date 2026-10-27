#pragma once
#include <snk/application/app_module_entry.h>
#include <entt/entity/fwd.hpp>
#include <entt/locator/locator.hpp>
#include <entt/meta/context.hpp>
#include <cstdint>
#include <string_view>

namespace desc {
	class desc_system;
}
namespace ecs {
	class system_factory;
}
namespace scn {
	class ecs_assembler;
}

// Contract between the spike game module (spike_game, a SHARED library loaded with dlopen /
// LoadLibrary) and the host test (#218). The host includes this header only to name the game's
// component in its checks; the engine itself never sees these types.
namespace spike {

	// Defined by the game. The module creates its storage first (spawner, sandbox).
	struct velocity_component {
		float dx = 0.f;
	};

	// Never named by host code before the module created its storage.
	struct module_only_component {
		int value = 0;
	};

	inline constexpr std::string_view velocity_desc_type = "spike_velocity_desc";
	inline constexpr std::string_view move_system_name = "spike::move_system";

	// What the host hands the module in app_data_storage (looked up by ds::type_id across the boundary).
	struct host_services {
		host_services(desc::desc_system& d, scn::ecs_assembler& a, ecs::system_factory& s, bool share)
			: descs(d), assembler(a), systems(s), share_meta(share)
		{}

		desc::desc_system& descs;
		scn::ecs_assembler& assembler;
		ecs::system_factory& systems;
		// false: the module keeps registering meta in its own entt::locator<meta_ctx> (the default).
		bool share_meta = true;
	};

	// Identities as the module computes them, compared by the host with its own.
	struct module_probe {
		entt::id_type velocity_hash = 0;
		entt::id_type velocity_index = 0;
		std::string_view velocity_name;
		const void* velocity_info = nullptr;
		entt::id_type engine_hash = 0; // scn::local_transform
		entt::id_type engine_index = 0;
		std::uint64_t desc_system_key = 0; // ds::type_id::value<desc::desc_system>()
		const void* meta_ctx = nullptr;    // the module's entt::locator<entt::meta_ctx>::value_or()
		bool standard_cpp = false;         // ENTT_STANDARD_CPP seen by the module
	};

}

// The module's extra C entry points (besides SNK_DECLARE_MODULE).
using spike_probe_fn = void (*)(spike::module_probe*);
using spike_spawn_in_sandbox_fn = void (*)(entt::registry*, float);
using spike_emplace_module_only_fn = void (*)(entt::registry*, entt::entity, int);
// Resolves an engine type (scn::depth_level) through the module's current meta context and returns
// &meta_type::info(): an implicitly created meta node.
using spike_resolve_engine_type_fn = const void* (*)();
