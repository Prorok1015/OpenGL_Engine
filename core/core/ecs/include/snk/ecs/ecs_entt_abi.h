#pragma once
#include <entt/config/config.h>
#include <entt/core/fwd.hpp>
#include <cstdint>
#include <type_traits>

// EnTT ids cross binaries (a game module and snk_engine) as entt::type_hash, which needs the pretty-function names:
// under ENTT_STANDARD_CPP it becomes a per-binary counter, and a different ENTT_ID_TYPE changes the layout of every
// registry and meta container (#218, docs/roadmap/design/SPIKE-218-entt-dll.md). Every ECS header that includes
// EnTT includes this first; the root CMakeLists.txt checks the build flags as well.
#if defined(ENTT_STANDARD_CPP)
#error "ENTT_STANDARD_CPP is not supported: entt::type_hash must be stable across the engine and game modules"
#endif
static_assert(std::is_same_v<entt::id_type, std::uint32_t>, "entt::id_type is part of the engine ABI; do not redefine ENTT_ID_TYPE");
