# Snake Engine

Snake Engine is a modular C++ game engine designed with a focus on flexibility, extensibility, and a powerful integrated editor.

## Getting Started

The project is optimized for the **Project IDX** environment. All dependencies, including compilers, build tools, and essential libraries, are pre-configured in `.idx/dev.nix`.

### Building the Project

The engine uses CMake for its build system. To build:

1.  **Initialize build directory:** `mkdir build && cd build`
2.  **Configure:** `cmake ..`
3.  **Build:** `make -j$(nproc)`

This produces the `editor` executable in the `Editor/` subdirectory of your build folder.

### Running the Editor

From the `build` directory:

```bash
# Using a config file
./Editor/editor --config ../Editor/config/editor.cfg

# Or overriding the resource path directly
./Editor/editor --set resource/path ../Editor/res
```

---

## Core Architecture & Concepts

### 1. Module System & Dependency Injection
The engine is built on a modular architecture (`app::modules::module_interface`). Modules are categorized by priority (CORE, ENGINE, EDITOR, GAME) and are responsible for:
*   **Registering Services:** Adding functionality to the global `app_data_storage`.
*   **Initializing Services:** Setting up cross-module dependencies.
*   **Lifecycle Management:** Clean shutdown and resource release.

### 2. Service Locator (`app_data_storage`)
Located in `core/core/common/ds/ds_store.hpp`, this is a type-safe container used for dependency injection. It allows modules to:
*   `construct<T>(args...)`: Register and instantiate a service.
*   `require<T>()`: Access a service by its type.
*   Manage service lifetimes using shared ownership policies.

### 3. Configuration System
The engine features a robust, macro-based configuration system (`cfg_api.h`):
*   **Define variables:** `CFG_VAR_DEF_INT(my_var, "path.to.var", 42);`
*   **Access variables:** Use `*my_var` to get the value.
*   Variables can be updated via `.cfg` files or command-line arguments (`--set path=value`).

### 4. Entity-Component-System (ECS)
A custom ECS implementation (`core/core/ecs`) provides a highly performant way to manage game objects and their behavior through data-oriented design.

### 5. Descriptor System (`desc`)
A unique system for defining resource properties and metadata using `.desc` files. It bridges the gap between raw data (like textures or meshes) and engine-ready resources.

---

## Project Structure

### `core/core` (The Foundation)
*   **`application`**: Core loop and module orchestration.
*   **`common/ds`**: Advanced data structures:
    *   `ds_event`: Type-safe event system.
    *   `ds_fixed_vector`: High-performance, fixed-capacity container.
    *   `ds_rtree`: Spatial indexing for fast geometric queries.
    *   `ds_bbox`: Bounding box utilities.
*   **`common/logger`**: Centralized logging system.
*   **`resource`**: Virtual File System (VFS) and resource loading/caching.
*   **`video`**: Graphics API abstraction layer (RHI).
*   **`windows`**: Windowing and OS integration.

### `core/engine` (The High-Level Systems)
*   **`render`**: The main rendering pipeline, shader management, and geometry handling.
*   **`scene`**: Scene graph, transformations, cameras, and lighting.
*   **`gui`**: Integration with ImGui for editor and debug overlays.
*   **`input`**: High-level input mapping and event handling.

### `Editor`
A comprehensive toolset for scene editing, asset management, and live debugging.
*   **`edt_guizmo`**: 3D manipulation tools.
*   **`edt_file_dialog`**: Integrated file management.
*   **`edt_spawn_system`**: Logic for instantiating entities in the scene.

---

## Dependencies
Installed automatically via Nix:
*   `cmake`, `gcc`, `gdb`, `pkg-config`
*   `assimp` (Model loading)
*   `xorg` libraries (X11, RandR, Inerama, Cursor, Xi)
*   Internal: `glad` (OpenGL loading), `stb_image` (Image parsing).
