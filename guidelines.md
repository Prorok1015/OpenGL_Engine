# Snake Engine Coding Guidelines

This document defines the coding standards, architectural patterns, and library configurations for developers and AI agents working on the Snake Engine.

## 1. Code Style

### Naming Conventions
*   **Namespaces**: Short, 3-4 letter abbreviations (e.g., `ds`, `ecs`, `scn`, `gui`, `cfg`, `wnd`, `inp`, `edt`).
*   **Classes & Structs**: `snake_case` (e.g., `data_storage_t`, `camera_component`).
*   **Functions & Methods**: `snake_case` (e.g., `register_services`, `require_shared`).
*   **Variables**: `snake_case`. Private member variables should end with an underscore (e.g., `is_initialized_`).
*   **Enums & Macros**: `UPPER_CASE` with underscores (e.g., `MODULE_PRIORITY`, `CFG_VAR_DEF_INT`).
*   **Files**: `snake_case` with prefixes corresponding to their module (e.g., `ds_store.hpp`, `scn_renderer.cpp`, `wnd_window.h`).

### Formatting
*   **Indentation**: Use **Tabs** (size 4).
*   **Braces**:
    *   Functions: Opening brace on a **new line**.
    *   Control flow (`if`, `for`, `while`): Opening brace on the **same line**.
*   **Headers**: Always use `#pragma once`.
*   **Includes**: Order should be:
    1.  Main module header (for `.cpp` files).
    2.  Standard library headers (`<vector>`, `<memory>`).
    3.  Third-party libraries (`<imgui.h>`, `<glm/glm.hpp>`).
    4.  Project headers (`"ds/ds_store.hpp"`).

---

## 2. Architectural Patterns

### Service Locator & DI (`app_data_storage`)
The engine uses `ds::app_data_storage` for dependency injection.
*   **Accessing Services**: Use `data.require<T>()` for mandatory services and `data.require_shared<T>()` when the service might be optional or its shared pointer is needed.
*   **Service Registration**: Services must be registered during the `register_services` phase of a module.

### Module Lifecycle
Modules must implement `app::modules::module_interface`.
*   `register_services`: Register types in the storage.
*   `initialize_services`: Fetch dependencies from storage and perform setup.
*   `shutdown_services`: Clean up in reverse order of initialization.

---

## 3. ECS (Entity-Component-System)
*   **Components**: Plain Old Data (POD) structs. Use the suffix `_component` if the name isn't inherently descriptive (e.g., `name_component`, but `renderable` is fine).
*   **Systems**: Logic should be encapsulated in classes inheriting from `ecs::system_interface`.
*   **Registry**: Access the registry via the `ecs_system` service.

---

## 4. Configuration System
Use the `cfg` system for any tunable parameters.
*   Define variables using macros: `CFG_VAR_DEF_FLOAT(my_param, "category.param_name", 1.0f);`.
*   Externalize variables in headers using `CFG_VAR_EXT_*` if they need to be accessed across files.

---

## 5. Library Specifics

### ImGui (GUI & Editor)
*   Used for both engine debug UI and the Editor.
*   Follow the pattern of `ImGui::Begin()`, check return value, then `ImGui::End()`.
*   Editor logic resides in the `edt` namespace and `Editor/code/editor_system`.

### GLM (Math)
*   Standard for all linear algebra.
*   Include only what is needed (e.g., `<glm/vec3.hpp>`).
*   Use `scn_glm_json_convert.h` for serializing GLM types.

### Resource Management
*   Always use the `resource` system (`core::res`) for loading files.
*   Resources are identified by `res_tag`.
*   Use `desc` system for high-level resource definitions (materials, textures).

---

## 6. Development Philosophy
*   **Prefer `fixed_vector`**: For performance-critical code where the maximum size is known.
*   **Assertions**: Use `ASSERT_MSG` and `ASSERT_FAIL` from `engine_assert.h` liberally to catch logic errors early.
*   **Logging**: Use `engine_log.h` for tracing and error reporting. Avoid `std::cout`.
