#pragma once
// Precompiled header for project targets (SNK_PCH, cmake/SnkPch.cmake). Only third-party and
// standard-library headers: nothing from the project, so one PCH can serve every target.
// Headers that need per-target defines (glm/gtx, GLFW, Boost link macros) are deliberately absent.
// Never snk/game/gs_module_layout.h: it sets the layout key SNK_DECLARE_MODULE exports (#614), and
// AModuleThatDoesNotKnowTheLayoutsReportsZero relies on a TU without it.

// standard library
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <queue>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// third-party
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <entt/entt.hpp>
#include <boost/json.hpp>
