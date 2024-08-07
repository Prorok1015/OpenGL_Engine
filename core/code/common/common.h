#pragma once
#include <GLFW/glfw3.h>

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <algorithm>
#include <functional>
#include <vector>
#include <array>
#include <queue>
#include <ranges>
#include <string>
#include <iomanip>
#include <numbers>
#include <filesystem>
#include <type_traits>

#include "engine_assert.h"
#include "logger/engine_log.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "ds_event.hpp"

using namespace std::string_literals;

template <typename SIGNATURE>
using EventManaged = ds::Event<SIGNATURE, ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>>;

template <typename SIGNATURE = void()>
using Event = ds::Event<SIGNATURE, ds::EventPolicySimpleContainer<ds::EventStoragePopicyVector>>;
