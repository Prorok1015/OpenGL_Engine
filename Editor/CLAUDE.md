# Editor — CLAUDE.md

## Architecture

```
Editor/
├── main.cpp                          # Module bootstrap: Core → Engine → Editor
├── config/editor.cfg                 # resource.path = ../../projects/sandbox/res/ (without --project)
├── data/                             # edt:// (editor.data_path): icons, templates (editor-only)
└── code/editor_system/
    ├── core/                         # Module lifecycle & frame loop
    │   ├── editor_module.cpp/h       # register/initialize/shutdown lifecycle
    │   ├── edt_editor_init.cpp/h     # constructs editor_system; adds the `ui` stage body and the editor_redirect_cameras stage
    │   └── edt_camera_redirect.cpp/h # redirects backbuffer cameras to textures (editor shows them in the viewport)
    │
    ├── controller/                   # Editor controllers (decomposed from editor_system)
    │   ├── edt_editor_system.cpp/h   # composition root: creates the services and editor_context, panels, wires callbacks, registers the built-ins, async orchestration
    │   ├── edt_scene_editor.cpp/h    # entity CRUD, desc tree helpers, serialize_and_push
    │   ├── edt_level_controller.cpp/h # new/load/save/quicksave level, write_to_disk
    │   ├── edt_world_controller.cpp/h # switch/create world
    │   ├── edt_scene_document.h/.cpp # scene_document: the level being edited, dirty flag, commit(), undo history
    │   ├── edt_prefab_document.h/.cpp # prefab_document: read-only prefab preview, owns the preview:<tag> set
    │   ├── edt_editor_camera.cpp/h   # camera state POD, save_camera_state / inject_camera
    │   └── edt_editor_layer.cpp/h    # ImGui layer: dockspace (main menu + default layout from the editor_context registries)
    │
    ├── panels/                       # 7 UI panels, all extend panel_base
    │
    ├── edt_component_renderers/      # per-type ImGui renderers for components
    │   ├── edt_component_renderers.cpp/h
    │   └── edt_cr_*.cpp              # skin, animation (camera, light and skybox are drawn from their schemas)
    │
    ├── import/                       # Asset import/export pipeline
    │   ├── edt_model_importer.cpp/h
    │   ├── edt_asset_exporter.cpp/h
    │   ├── edt_asset_export_dialog.cpp/h
    │   ├── edt_async_import_task.h   # import_intermediate: background-thread import result
    │   └── edt_filesystem_assimp_io.cpp/h
    │
    ├── gizmo/                        # Transform gizmo
    │   ├── edt_transform_gizmo.hpp
    │   └── edt_guizmo.hpp
    │
    ├── game_build/                   # Build > Build Game (#434)
    │   ├── edt_build_diagnostics.h   # build_output_parser (MSVC/GCC/Clang/ld/CMake lines -> problems)
    │   ├── edt_build_settings.cpp/h  # build_settings: <root>/.snk/build.json (parse/merge/load, update_build_settings read-merge-write, pure)
    │   ├── edt_build_settings_model.cpp/h # build_settings_model (Build Settings state, no ImGui), preset_for_configuration, build.settings.get (#444)
    │   ├── edt_build_settings_panel.cpp/h # build_settings_panel: "Build Settings" window (Build > Build Settings...)
    │   ├── edt_path_text.h           # utf8_of(path), shared by the game_build files
    │   ├── edt_toolchain.cpp/h       # find_toolchain: cmake + ninja + environment (vswhere/vcvars64 on Windows), pure parsers
    │   ├── edt_build_jobs.cpp/h      # recommended_jobs(cores, ram, compiler): (ram - 2 GiB) / (2 GiB MSVC, 1.5 GiB GCC/Clang) clamped to [1, cores]; estimate_jobs() (#444)
    │   ├── edt_game_builder.cpp/h    # game_builder: action build.game_module, the build job, its progress window (not modal)
    │   └── edt_game_reloader.cpp/h   # game_reloader: action build.reload_game_module, messages for a build not taken (#435)
    │
    ├── vcs/                          # git (#454)
    │   ├── edt_vcs_parse.cpp/h       # pure parsers: status --porcelain=v2 -z, log -z, for-each-ref, stderr classification, valid_utf8
    │   └── edt_vcs_service.cpp/h     # vcs_service: git as job_service jobs; vcs.info/status/diff/log/branches, stage/unstage/commit/branch.create/checkout, fetch/pull/push
    │
    ├── input/                        # Input binding helpers & file dialog
    │   ├── edt_input_manager.h       # make_input_area / to_framebuffer_size (window units -> framebuffer pixels)
    │   ├── edt_input_binding.h       # bind_item_input: a hovered panel image binds its rect to a context in inp::input_router
    │   └── edt_file_dialog.cpp/h
    │
    └── CMakeLists.txt
```

## Controller Decomposition

`editor_system` was split into 4 classes connected via `scene_document` and callbacks:

| Class | File | Responsibility |
|-------|------|----------------|
| `editor_system` | `controller/edt_editor_system.cpp/h` | Thin coordinator: init, panel wiring, camera, async orchestration |
| `scene_editor` | `controller/edt_scene_editor.cpp/h` | Entity CRUD (create/delete/duplicate/reparent), desc tree helpers, `serialize_and_push`, `sync_ecs_transforms_to_desc` |
| `level_controller` | `controller/edt_level_controller.cpp/h` | Level I/O: new/load/save/quicksave, `write_level_to_disk`, `populate_worlds_from_level`, templates |
| `world_controller` | `controller/edt_world_controller.cpp/h` | Multi-world: `switch_to_world`, `create_world` |

**Documents** (`include/snk/editor/edt_document.h`, `edt_document_manager.h`; `editor_context::documents()`): `document` has `id/kind/title/source/is_dirty/generation/can_save/save/save_as`. `document_manager` holds the one `scene_document` (owned by `editor_system`, the controllers are built on it) and at most one `prefab_document` preview: `scene()`, `active()`, `set_active(id)`, `open_prefab(tag)`, `preview()`, `close(id)` (the scene cannot be closed), `for_each`. It raises `document_opened/closed/activated`.

**`scene_document`** (`controller/edt_scene_document.h`) holds mutable state shared between controllers:
- Level identity: `editor_tag`, `level_tag`, `level_name`; the dirty flag is `is_dirty()` / `mark_clean()`
- Multi-world: `world_descs`, `world_names`, `worlds_systems_list`, `active_world_idx`
- `camera` (editor camera state, survives the world reload)

**`editor_services`** (`include/snk/editor/edt_editor_services.h`): `ctx_manager` (weak_ptr), `level_set()` (the `level` context set), `active_layer()` (the context manager's game layer) and the game services `descs()`, `resources()`, `assembler()`, `sfactory()` — the active layer's, else the `base_*` ones of the `app_data_storage`. Owned by `editor_system` (declared before the controllers, filled in `init()`); the controllers, `prefab_document`, `document_manager`, the override `view_cache`, the state commands and `editor_context` take it by reference. It lives as long as `editor_system`, not as long as a document. **Never keep a game service taken from it**: a game module replaces the game layer, so take them at the moment of use. Store and erase through `writable_resources()` (the base `resource_system`): a module's layer only writes under `memory://modules/<name>/`. The editor's own tools (model importer, asset exporter, icons, templates) stay on the base `desc_system`/`resource_system` of `editor_system`.

**Every edit ends in `scene_document::commit(label, mode)`**: it marks the document dirty, bumps `desc_generation`, pushes the descs into the level (`scene_editor::serialize_and_push`, installed by `scene_editor` through `set_pusher`) and emits `events.document_changed(id, generation)`. A push that finds no level emits nothing. `commit_mode::state_only` skips the push and the announcement (new level, new world: the contexts are already built; nothing was announced before either). **Undo history** (#141-#144, `snk/editor/edt_document_history.h` + `controller/edt_scene_history.h`, design in `docs/epics/editor/EPIC-15.md`): a push commit diffs the worlds against the last commit by node id and records one `history_change` (keys `node:<world>:<id>`) under `edit_origin()` (`edit_origin_scope` for an agent); `is_dirty()` = something unrecorded or not at the save point (`mark_clean()`); `undo()/redo()/rollback_group()` apply it and rebuild every world they changed (the active one through the pusher, others through `set_world_reloader` = `scene_editor::reload_world`; `set_before_history_apply` runs first: pointers into the worlds go stale). A change that only moved a node (positional) has the node's key but no copy of it, and only moves it; a rollback stops at a shared key or at `history_change::undo_conflict()` (a node someone put under a node the change created). `state_only`, a level load/reload/detach (`reset_history()`) are boundaries; the live transforms written back before a save are `rebase_history()`. An edit the diff cannot compare (a node without an id, an id twice) clears the history and stays dirty. `~editor_system` calls `document_manager::shutdown()` (closes the preview, drops the scene) because the context outlives the system. Never set the dirty flag or call `serialize_and_push` from an edit directly. Save / Save As go through `document::save()/save_as()` (handlers set by `editor_system`); the File menu and the exit prompt still go through `file_menu` and `dialog_service`.

**`prefab_document`** (`controller/edt_prefab_document.h`): read-only (`can_save()` false), opened with `document_manager::open_prefab`, owns the set `preview:<tag>` while open.

Controllers communicate via `std::function` callbacks set by `editor_system` during `init()`.

## Panels

| Panel | Header | Purpose |
|-------|--------|---------|
| `scene_hierarchy_panel` | `edt_scene_hierarchy_panel.h` | Prefab node tree + world tabs; callbacks for create/delete/rename/duplicate/reparent |
| `inspector_panel` | `edt_inspector_panel.h` | Component property editor; custom renderer from `inspector_registry`, else the UI generated from the desc schema (`edt_schema_inspector.h`), else generic JSON |
| `viewport_panel` | `edt_viewport_panel.h` | 3D view + transform gizmo + orientation gizmo; fires `on_transform_committed` |
| `game_view_panel` | `edt_game_view_panel.h` | "Game view" (View menu; `editor/show_game_view` opens it at startup): camera picker + image of the picked game camera; the cameras that draw into the picked camera's texture (the worlds of one set share it, `camera_redirector`) get the viewport, all other game cameras are switched off (logic in `edt_game_view_logic.h`, unit-tested) |
| `console_panel` | `edt_console_panel.h` | `add_log(level, msg)`, max 10K entries with 25% trim, counts per level, filter, clear |
| `problems_panel` | `edt_problems_panel.h` | "Problems" (open by default, docked with the console): table of `editor_context::problems()` (severity, file:line relative to the project root, message; notes in the row's tooltip, rows clipped with `ImGuiListClipper`), counts per severity as filters, "Copy" in the row's context menu |
| `blackboard_panel` | `edt_blackboard_panel.h` | "Blackboard" (View menu, closed by default): read-only table of the committed blackboard of a context set (key, value, type, writer) with a set picker and a key filter; the rows and the value text are `edt_blackboard_view.h` (no ImGui, unit-tested) |
| `asset_browser_panel` | `edt_asset_browser_panel.h` | Directory tree + file grid; drag-drop to viewport |
| `dockspace` | `edt_dockspace.h` | ImGui DockSpace + default layout; the menu bar is `draw_main_menu` (`edt_main_menu.h`) |
| `panel_base` | `edt_panel_base.h` | `render()` wraps ImGui::Begin/End; `virtual on_render()` |

All panels are `shared_ptr` members of `editor_system` and are registered in `editor_context::panels()` (`panel_registry`: id, title, `dock_slot`, `open_by_default`; registration order = render order = View menu order). `dockspace` calls `render_all()` each frame. Menu items and shortcuts are `editor_context::actions()` (`action_registry`); `file_menu::register_actions` adds the File items. Every action has `args_schema`/`result_schema` (JSON Schema subset, `snk/command/cmd_json_schema.h`) and runs through `action_registry::invoke(id, json)`; it never reads ImGui input - the menu click or shortcut runs `activate`, which collects arguments via `dialog_service` and then calls `invoke`. `add()` returns a RAII `registration` (keep it as long as the entry should exist). `action_registry` owns a `cmd::command_registry` (engine module `snk::command`: id, schemas, `enabled`, `invoke`) and keeps the editor-only part (label, menu, shortcut, `activate`) in a side table keyed by id; `commands()` is the engine registry. File menu: Save Level (`file.save`, Ctrl+S) overwrites the level, or opens Save As if it has no path; Save Level As... (`file.save_as`, Ctrl+Shift+S) always asks for a path. The window's own close request (X, Alt+F4) is vetoed and goes through `file_menu::exit` (`attach_window_close`), so it asks about unsaved changes like File > Exit. In the editor that registry is the process-wide `cmd::command_registry` that `core_module` puts in `app_data_storage` (engine commands such as `log.read` are registered there; `editor_context` passes it to `action_registry(shared)`); `action_registry()` alone owns a private one (tests).

Services seen by scripts and agents (A2, `edt_service_commands.h`): `job_service` raises `editor_events::job_started/job_progress/job_finished` (progress is polled once per frame by `job_service::pump()` in the `editor_jobs` frame stage) and serves `job.list`, `job.get{id}`, `job.cancel{id}` (a `done` job started with `start_with_result` keeps the JSON its work returned as `job_info::result`: `job.get` shows it, `job_finished` carries it as its 4th argument; the kept results stay under `job_service::RESULT_BUDGET` (16 MiB serialized): the oldest are dropped first, the newest always stays, and a dropped one reads `result_dropped: true`); every `dialog_service` request has an id, a kind and an answer schema (`list()`), and `dialog.list` / `dialog.answer{id, answer}` resolve it through the same callback a click uses. The engine log is a ring journal (`engine::read_log_journal`, command `log.read`); `engine::add_log_sink` (base-only `<snk/common/logger/engine_log_sinks.h>`) returns an RAII handle (the console panel is one sink, `editor_system::m_log_sink`). `problem_list` (`edt_problem_list.h`, `editor_context::problems()`) holds the problems per source (`set(source, items)` replaces that source's set, `clear(source)`), raises `editor_events::problems_changed(source)` and serves `problems.list{source?, severity?}` (allowed to agents by id); `build_output_parser` (`game_build/edt_build_diagnostics.h`) turns build output lines into `problem`s.

**Building the game module** (#434, `game_build/edt_game_builder.h`): `game_builder` (created by `editor_system::init`, destroyed before the context) registers `build.game_module` (Build > Build Game, Ctrl+B; args `{configuration?: "debuggame"|"release"}`, result `{started, job}`; a second call while a build runs fails with `busy`; `risk = deny_by_default`, so `agent_may_use` keeps it from MCP agents: it runs the project's CMake code). Enabled only with a project open, `<root>/code/CMakePresets.json` present and the shared engine (`SNK_ENGINE_SHARED`). The job (`ds::lane::io`, `job.cancel`, Cancel or Esc in its non-modal progress window) finds the toolchain (`edt_toolchain.h`: Windows = vswhere -> `cmd /u /d /v:off /s /c call <vs>\VC\Auxiliary\Build\vcvars64.bat >nul 2>&1 && set` read raw (`process_options::raw_output`) as UTF-16LE, a path with `& | < > ^ % !` refused, + `VSLANG=1033`, cmake/ninja of VS unless on PATH; Linux = own environment + `LC_ALL=C`, cmake/ninja on PATH; cached per session, failures not), then runs `cmake --preset <preset>` and `cmake --build --preset <preset> [--parallel N] [--verbose]` in `<root>/code` with `SNAKE_SDK_DIR`; every line goes through `build_output_parser` into `problems()` source `build` (cleared at the start of a `dialog` build; a `problems_only` build replaces them only when it ends, so a restarted or cancelled background build leaves them as they were), ninja's `[n/m]` is the progress. A failure shows a message dialog; a cancel shows nothing. Processes run through an injected runner (`platform::run_process` by default), so the tests fake it. cfg: `build/sdk_dir` (else `<exe>/../lib/cmake/snake_sdk`, else `SNK_BUILD_TREE_SDK_DIR` = `<build>/snake_sdk`), `build/cmake`, `build/vs_path`, `build/vs_min_version` (17.14), `build/configuration` (debuggame). Per build everything comes from a `build_request` (`configuration`, `preset` (empty = configuration), `jobs` (0 = no `--parallel`), `priority` (`low` = `process_options::low_priority`: BELOW_NORMAL / nice 10 for the process group), `cancel_on_restart` (stops the running build and runs once it stopped, latest request only; else `busy`), `preemptible` (a background build: a request that is not preemptible stops it the same way instead of being `busy`; a preemptible request meeting the person's build, running or waiting, is `busy`; `building_preemptible()` / `is_manual_build_running()` tell the two apart), `verbosity`, `reload_after`, `report` (`dialog`; `problems_only`: no message dialog, the failure tail and the start/built lines at debug, problems as always), `show_progress` (false: opens no progress window, one already open stays)) given to `game_builder::start`; the action makes it with `make_manual_build_request` from `options.settings()` (read at every start; the editor loads `<root>/.snk/build.json`: `configuration`, `jobs_mode` auto|manual, `reload_after_build`, `build_before_play` (stored only), `log_verbosity` errors|normal|verbose, `live_enabled`, `live_quiet_ms` (3000, 0..600000), `live_jobs` (0 = auto, 0..1024); unknown keys kept by `merge_build_settings`, bad values warn and default), `options.preset_for` and `options.auto_jobs` (manual jobs, or no `auto_jobs`, = 0: the preset decides). `make_live_build_request` (#445, live mode): configuration as without an argument, `live_jobs` else min(2, `auto_jobs`) (at least 1), low priority, `cancel_on_restart`, settings' verbosity, `problems_only`, no progress window, `reload_after` false, `preemptible` (Build Game stops it: owner decision), fresh as manual. A build stopped for a restart finishes its job as `done`. Configuration: argument > build.json > cfg. Verbosity sets the level of the output tail on success (errors: none, normal: debug, verbose: info; a failure is always warn). After a successful build `options.events->game_module_built` gets `{module, path (absolute build/bin file that exists), configuration, job, reload}` per runtime module; never after a failure, cancel or restart. **Reloading the game module** (#435, `game_build/edt_game_reloader.h`): `game_reloader` registers `build.reload_game_module` (Build > Reload Game Module; args `{module?}`, result `{status, message}`; `deny_by_default` like the build) and `editor_system` reloads on `game_module_built` (when `reload`) on the next UI frame. Both go through `core::project_module_host::reload` (put in the storage by `main`): the copy is checked next to the running module, then swapped; a failed build rolls back to the previous copy. Around the swap `module_swap_gate` (`controller/edt_module_swap_gate.h`; `editor_system` routes the game layer changes through `on_game_layer_change` and hands `hooks()` to the host) holds the level detached: it keeps its unsaved edits and is built once, in the new layer. A refused or rolled-back build is shown as a message. Rule of the module API: nothing of a module's image (an exception its code threw, an object, a string, a vtable) may outlive the unload of its library; `module_loader` copies a module exception's message while the code is loaded and throws the host's own `std::logic_error`. Presets (#444, `<snk/project/prj_cmake_presets.h>`): the editor never writes `code/CMakePresets.json`; per-user defines/jobs go to its own `user-<cfg>` presets in `code/CMakeUserPresets.json` as an edit list re-applied to the file read fresh at save (`save_user_presets`), keeping everything else. `preset_for` = `preset_for_configuration` (`user-<cfg>` when CMakeUserPresets.json has it; a broken file warns and gives `<cfg>`), `auto_jobs` = `estimate_jobs().jobs`. **Build Settings** (`build_settings_panel`, floating, closed by default; action `build.settings.open`, Build > Build Settings..., not a tool): team defines of CMakePresets.json read-only, the user's defines and jobs (Manual: `jobs` of every `user-<cfg>` build preset) in CMakeUserPresets.json, default configuration, module reload, log verbosity; `build_before_play` hidden until Play exists. All state is `build_settings_model` (load / queued edits validated by `check_user_define` / `dirty`, `revert`, `pending_files`, `changed_on_disk` / `apply`: build.json first via `update_build_settings` (only the edited fields, merged onto the file as it is now; a broken build.json is never overwritten), then the user presets via `save_user_presets`; atomic writes through `prj::write_text_atomically`, or the `set_writer` fake in tests). While a build runs (`set_building`) a define cannot be removed nor a removal applied: that build's configure would clear the new mark. A mark cleared on disk is taken over silently (`refresh_fresh_configure`). Removing a user define of `<cfg>` adds `<cfg>` to build.json `fresh_configure` (CMakeCache.txt keeps a removed define, and `user-<cfg>` shares the build tree): the next build configures with `cmake --preset <p> --fresh` (`build_request::fresh`), and `options.on_fresh_configured` (wired to `clear_fresh_configure`) drops it after a configure step that succeeded. Agents read the same files through `build.settings.get` (incl. `live: {enabled, quiet_ms, jobs}`; read-only, allowed by id; `no_project` without a project); there is no set.

**Version control** (#454, `vcs/edt_vcs_service.h`): `vcs_service` (created by `editor_system::init` with `context.jobs()` and the project root) finds git at start (cfg `vcs/git`, else `git`/`git.exe` on PATH; `git --version`) and the repository (`git rev-parse --show-toplevel` from the project root, cached per project root). Both probes run on the main thread with a hard timeout (`vcs_options::probe_timeout`, 5 s: a `std::jthread` watchdog stops git; then `vcs_unavailable`, "git did not answer within 5 s", WARN). Without git, a project or a repository it is unavailable with a reason and `code` `vcs_unavailable` / `no_project` / what git said (`not_a_repository`, ...); the editor keeps running. Only `vcs_unavailable` and `no_project` make the commands refuse at once; anything git reports (a folder that is not a repository included) comes back as the job's result. Git runs through an injected `process_runner` with the editor's environment minus `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`, `GIT_ALTERNATE_OBJECT_DIRECTORIES`, `GIT_COMMON_DIR`, `GIT_NAMESPACE`, `GIT_CONFIG_PARAMETERS`, `GIT_CONFIG_COUNT`, `GIT_CONFIG_KEY_*`/`GIT_CONFIG_VALUE_*`, `GIT_EXTERNAL_DIFF`, a non-empty `GIT_EXEC_PATH`, plus `LC_ALL=C`, `GIT_TERMINAL_PROMPT=0`, `GCM_INTERACTIVE=never`, `GIT_SSH_COMMAND=ssh -oBatchMode=yes` (only when unset), stdout raw and stderr apart (`process_options::on_stderr_line`). **A read never runs a program the repository's config names** (a project unzipped with a hostile `.git/config` is code execution otherwise): every call (writes too) has `--no-pager -c core.quotepath=off -c core.fsmonitor=false -c diff.external= -c log.showSignature=false -c status.submoduleSummary=false -c diff.ignoreSubmodules=all --no-optional-locks --literal-pathspecs`, a read also `-c core.hooksPath=/dev/null (NUL)`; every read job first lists the filter drivers (`git config -z --get-regexp ^filter\.`, all scopes, read again per job so a config edit is seen) and passes `-c filter.<X>.clean= .smudge= .process= .required=false` for each and for `lfs` (a driver name with `=` or a control character cannot be overridden: the read ends with `unsafe_repository`); diff adds `--no-ext-diff --no-textconv`, status and diff `--ignore-submodules=all`, log `--end-of-options <ref> -- [path]`. status, log and branches stop git past 4 MiB and keep whole records (`truncated`). Covered by `AHostileRepositoryConfig_RunsNothingOnReads` (fsmonitor, textconv, diff driver, clean filter). Every git call is a `job_service::start_with_result` job on `ds::lane::io` (cancel = the stop token kills git): `vcs.status`, `vcs.diff{path?, staged?}` (256 KiB cap, `truncated`), `vcs.log{ref?, path?, limit<=200, cursor?}`, `vcs.branches` return `{job}` and the parsed answer is the job's `result` (`job.get`); a git failure is a `done` job with `result.error = {code, message, stderr_tail, exit_code}` (`code` from `vcs::classify_error`: `auth_required`, `unsafe_repository`, `not_a_repository`, `no_commits`, `bad_revision`, `network`, `locked`, `git_failed`, or `vcs_unavailable` when git could not start; `message` = git's last `fatal:`/`error:` line; `stderr_tail` = the last 20 stderr lines, at most 4 KiB), never a failed job (that would open an error dialog). `vcs.info` is sync. All five are read-only and allowed to agents by id. **Writes** (`mutates`, agent category `unset` until #455 (`// TODO(#455): vcs_* categories`), not in `read_commands`: agents see none), each `{job}` like the reads: `vcs.stage{paths}` (`git add -- <paths>`), `vcs.unstage{paths}` (`git reset -q -- <paths>`), `vcs.commit{message}` -> result `{hash}` (the message goes through `-F <file>` in the service's scratch folder, deleted after: the runner has no stdin), `vcs.branch.create{name, from?}`, `vcs.checkout{ref}` (`checkout -q <ref> --`, never forced), `vcs.fetch{remote?}`, `vcs.pull{remote?, branch?}` (in steps: `fetch`, the agent checks below, `-c submodule.recurse=false merge --ff-only --no-stat FETCH_HEAD`; never a rebase or a merge commit; nothing marked for merge in FETCH_HEAD = `git_failed`), `vcs.push{remote, branch, set_upstream?}` (never `--force`/`--force-with-lease`/`+refspec`); the others' result is `{output}` (git's last 20 lines). A ref/name/remote/path starting with `-`, a remote with `:` (a URL), a pull/push branch with a leading `+` or a `:` are `invalid_args` before any job; paths of one call at most `k_max_paths_bytes` (16 KiB). Submodules are not entered (`--no-recurse-submodules`). **Agent guard** (owner decision): a write whose `cmd::call::from.kind == agent` runs with `-c core.hooksPath=<empty folder>` (in a 0700 folder of the service under the OS temp dir, made once, emptied again if something wrote into it), `-c core.askPass= -c core.sshCommand=<GIT_SSH_COMMAND> -c protocol.ext.allow=never -c commit.gpgsign=false -c tag.gpgsign=false -c gpg.program= -c core.gitProxy=`, `GIT_PROXY_COMMAND=` (empty: git takes the first core.gitProxy it reads, so the -c alone would lose to the repository's) and without `GIT_ASKPASS`/`SSH_ASKPASS`. **Filters:** stage, unstage, commit, checkout and pull list the configured drivers (`git config --get-regexp '^filter\..*\.(clean|smudge|process)$'`, all scopes); if there are any, the files the write touches (stage/unstage: `ls-files -z --cached --others --exclude-standard -- <paths>` plus the paths; commit: `diff --cached --name-only --no-renames -z`; checkout/pull: `diff --name-only --no-renames -z HEAD <target>`, `ls-tree -r` of the target without HEAD; pull's target is FETCH_HEAD after its fetch) go through `check-attr -z filter` (16 KiB of paths per call), and a configured driver it names ends the job `done` with `result.error.code = "filters_require_opt_in"` (message names the drivers) before git runs: switching filters off would commit LFS files as raw bytes. A `.gitattributes` among a checkout's or pull's changed files refuses too (the target's attributes may differ), and so does a list git could not give. A driver no attribute names (a global `filter.lfs`) blocks nothing. A checkout of a name that resolves to no commit (`rev-parse --verify --quiet <name>^{commit}` fails; a tag or branch of that name wins, as in git) but is a branch of exactly one remote diffs against `refs/remotes/<remote>/<name>` (git then creates the tracking branch); none or several remotes: the name itself (fails closed). Agent network calls need git 2.26+ (`--show-scope`): older (by the probed `git --version`) is `vcs_unavailable` at once, "git 2.26 or newer is required for agent network operations (found X)". Not checked: remote helpers (`remote.<name>.vcs`, `<helper>::` URLs run `git-remote-<x>` from PATH; `ext::` is blocked), attributes and filters inside submodules (recursion is off). **Network** (fetch, pull, push of an agent): `config -z --show-scope --get-regexp` refuses with `unsafe_repository` a non-empty `credential.helper` / `credential.<url>.helper` outside the system and global scopes (an include of the local config reports `local`; message "a credential helper set by the repository"), a local/worktree `core.sshCommand`, a local/worktree `url.<base>.insteadOf`/`pushInsteadOf` whose base is not `https://`, `ssh://` or `user@host:`, and `remote.<name>.uploadpack`/`receivepack` (any scope) other than `git-upload-pack`/`git-receive-pack` for the remote used (named, else the branch's, else origin); git then gets those two forced with `-c`. `vcs_write_permissions{allow_hooks, allow_filters, allow_repo_credential_helper}` (`vcs_service::set_write_permissions`, all false; TODO(#455): from the policy) lift each. A person's write (ui, test, game) runs hooks, filters and helpers as plain git. Covered by `AHostilePreCommitHook_RunsForAPersonButNotForAnAgent`, `AnLfsStyleFilter_RefusesAnAgentsCommit_UnlessFiltersAreAllowed`, `ARepositoryCredentialHelper_RefusesAnAgentsPush`, `AFilterDriver_RefusesAnAgentsTreeWrite_OnlyWhereItApplies`, `AnAgentsNetworkCall_RefusesProgramsTheRepositoryConfigPicks`, `ACredentialHelperFromAnIncludeOfTheLocalConfig_RefusesAnAgentsPush`, `AnAgentsCheckoutOfARemoteOnlyBranch_IsCheckedAgainstTheRemoteBranch`, `AnAgentsCheckoutOfATagNamedLikeARemoteBranch_IsCheckedAgainstTheTag`, `AGitOlderThan2_26_RefusesAnAgentsNetworkCall`. No force-push, `reset --hard`, branch delete or rebase is ever registered. Every text from git goes through `vcs::valid_utf8`.

State queries for agents (A4, `edt_state_commands.h`, registered by `editor_context`): `scene.query`, `node.get`, `component.schema`, `asset.query`, `selection.get`, all read-only. A node is addressed by `ref = {context: world name, path: [16-char hex node ids]}`; `scene.query` pages through the world desc (`page_size` <= 200) with an opaque cursor bound to the document generation (`stale` after an edit). The MCP bridge allows them by command id (`read_commands` in `edt_mcp_bridge_host.cpp`), never by domain.

## Inspector and Asset-Kind Registries

```cpp
// Registration (edt_component_renderers.cpp, called once at init; keep the returned registrations):
registrations.push_back(context.inspectors().add<scn::camera_desc>([](prefab_comp_node& n) -> bool { ... }));

// Lookup (inspector_panel):
auto result = inspectors.invoke(type_name, comp_node);
// result == nullopt  → fall back to generic JSON editor
// result == true     → component was mutated, call on_node_changed
```

**Adding a new component renderer**: implement lambda in `edt_component_renderers/`, register it in `edt_component_renderers.cpp::register_desc_component_renderers()`. A second renderer for the same type is refused with an error.

`editor_context::asset_kinds()` (`asset_kind_registry`) maps a res-relative path suffix to `{open, drop_into_scene, icon}`; the longest matching suffix that has the wanted action wins. Built-in kinds (`prefab` = `.prefab.desc` opens the prefab editor, `desc` = any asset dropped onto the viewport is added to the scene) are in `controller/edt_builtin_asset_kinds.cpp`.

## Desc ↔ ECS Edit Loop

```
User edits in Inspector
  → inspector on_node_changed callback
  → scene_document::commit("Edit node") → scene_editor::serialize_and_push()
      → active_world_desc serialized to JSON
      → desc_system re-parses level desc
      → ecs_assembler re-assembles world registry
  → Viewport reflects changes next frame

Gizmo drag committed
  → on_transform_committed(node_id path, pos, rot, scale)   (path = scn::node_id_component of the entity)
  → scene_editor::resolve_node_path in active_world_desc → { owner, target }
  → target empty: update owner.position / rotation / scale
  → target non-empty (node inside a prefab instance): override_edit::write_transform → override_record on owner
  → scene_document::commit("Move entity")
```

The editor never finds nodes by name (names are display only): hierarchy, viewport and `scene_editor` pass `node_id`s / id paths. Inspector edits of a node inside a prefab instance go through `edt_override_edit.h` (records on the instance node; "Reset to base" drops them).

**ECS → Desc** (called before save): `scene_editor::sync_ecs_transforms_to_desc()` reads `local_transform` of entities with a `node_id_component`, resolves the path, and writes back to the (non-instanced) desc nodes.

## Level JSON Schema

```json
{
  "worlds": [
    {
      "name": "World0",
      "prefab_desc": { /* prefab_node tree */ },
      "systems": ["SystemA", "SystemB"]
    }
  ]
}
```

`level_controller::build_level_json()` is the single source of truth. `level_controller::populate_worlds_from_level()` reconstructs `scene_document::world_descs` on load.

## Multi-World

- `scene_document::world_descs` — vector, indexed by `active_world_idx`
- `scene_document::active_world_desc()` / `active_world_name()` — pointers, null when the level has no worlds (`active_world_idx = -1`, arrays empty; `world_controller::clear_active_world()`). Never index `world_descs[active_world_idx]` directly.
- Hierarchy panel shows a tab per world; switching calls `world_controller::switch_to_world(idx)` which saves editor camera, tears down old registry, rebuilds new one
- `scene_document::worlds_systems_list` — per-world list of system names (parallel to `world_descs`)

**Single source of truth for world order (critical invariant):** the three parallel
`scene_document` arrays (`world_descs` / `world_names` / `worlds_systems_list`) MUST be
built from the same ordered source — the desc-ordered world list. Do NOT build them by
iterating `scn::context_set::for_each`: it walks the contexts by **context id**, which fills
gaps left by removed worlds and so is not desc/insertion order, so with >=2 worlds the parallel arrays desync. Derive names and
per-world systems via `edt::derive_world_arrays` (`controller/edt_level_controller.h`), and
keep `populate_worlds_from_level` as the single build point on load. Covered by
`tests/edt_world_array_sync_tests.cpp`.

**Hot reload:** `scn::context_manager` drops/rebuilds only the `kind==level` contexts of the set `"level"`, so worlds created in the
editor but not yet saved survive. After every reload (world list or systems only) it calls `level_controller::resync_worlds_after_reload()`
(`merge_reloaded_worlds` + `derive_world_arrays`), which keeps the arrays aligned with the level. A reload
whose world desc fails to load is skipped entirely (current level kept, no callback).

## Prefab Node Format (critical)

```
prefab_desc::prefab_node {
  name, position, rotation(Euler deg), scale
  components: unordered_map<string, prefab_comp_node>
  children: vector<prefab_node>
}
prefab_comp_node { type_name, parent_desc, overrides(json::object) }
```

- `children` serialized as a JSON **array** of nodes `{ "__id", "name", "transform", "components", "children" }` (format v2; v1 keyed them by name and is migrated on load)
- `components` key = type_name (e.g. `"mesh_node_desc"`, `"bone_desc"`)
- `rotation` stores Euler degrees: `x=pitch, y=yaw, z=roll`; reconstructed via `yawPitchRoll(y,x,z)`

## Registered Component Descs (as of current state)

| Type string | Desc class | Spawner |
|-------------|-----------|---------|
| `prefab_desc` | `scn::prefab_desc` | `assemble_prefab` |
| `world_desc` | `scn::world_desc` | `assemble_prefab` |
| `camera_desc` | `scn::camera_desc` | `assemble_camera` |
| `directional_light_desc` | `scn::directional_light_desc` | `assemble_directional_light` |
| `skybox_desc` | `scn::skybox_desc` | `assemble_skybox` |
| `mesh_node_desc` | `scn::mesh_node_desc` | `assemble_mesh_node` |
| `skinning_desc` | `scn::skinning_desc` | `assemble_skinning` |
| `bone_desc` | `scn::bone_desc` | `assemble_bone` |
| `animations_desc` | `scn::animations_desc` | `assemble_animations` |
| `keyframes_desc` | `scn::keyframes_desc` | `assemble_keyframes` |
| `object_desc` | `scn::object_desc` | `assemble_object` |
| `anchor_desc` | `scn::scene_anchor_desc` | `assemble_scene_anchor` |

**Registration lives in**: `scn::register_scene_types` (`core/engine/scene/scn_registration_scope.cpp`)
**Adding a new desc type requires**: one `scope.desc<T>()` / `scope.spawner()` line there; unregistration is automatic when the returned `scn::registration_scope` is destroyed.

## Model Import Pipeline

Models (`.glb`, `.obj`, `.fbx`, `.gltf`) are loaded via `scn::model_importer_adapter` (only with `SNK_WITH_ASSIMP=ON`; with OFF there is no adapter, "Import Model" is disabled and loading such a file fails with a "No adapter found" log):
- Outputs `prefab_desc` (not legacy prototype)
- Geometry stored at `memory://path/name.geom.desc` (separate resource)
- Mesh nodes → `mesh_node_desc` components referencing geometry by tag
- Skinning → `skinning_desc` + bone weights registered with `rnd::skinning_manager`
- Bone nodes → `bone_desc` components with offset matrix + global index
- Animations → `animations_desc` on root, `keyframes_desc` on each animated node

Import in editor: File menu → `dialog_service::pick_file` → `editor_system::start_import` → async import → export dialog → auto-add to scene.

## File Dialog

```cpp
file_dialog.add_extension_filter(".glb");
if (file_dialog.show("Title", &is_open)) {
    // file selected, file_dialog.get_selected_path() available
}
// show() returns true exactly once when user confirms
```

Non-blocking ImGui modal — must be called every frame while open.

## Viewport & Gizmo

- Viewport shows the editor camera texture (`__editor_camera_rt`); game cameras (no color target = backbuffer) are redirected to their own textures by the `editor_redirect_cameras` stage (before `extract`)
- `ImTextureID` obtained via the injected `gui::gui_system&` (from `app_data_storage`, no global accessor): `gui.get_backend_interface()->to_imgui(handle)` with a `texture_handle` (`texture_manager::binding(id_of(tag)).handle` or `render_targets::registry::current(tag)`)
- Gizmo modes: Translate / Rotate / Scale (keyboard shortcuts T/R/S)
- Transform committed only on gizmo release (not every frame)
- `edt_transform_gizmo.hpp` — pure ImGui DrawList implementation, no external gizmo lib

## Common Pitfalls

- **EnTT `registry::each()` does not exist** — use `registry.storage<entt::entity>()` to iterate all entities
- **EnTT forward declare** — use `#include <entt/fwd.hpp>`, not `namespace entt { class registry; }`
- **Input routing**: ECS input is routed by `inp::input_router`; a panel binds its hovered image every frame (`bind_item_input`), the context under the cursor gets the full `inp::input_state`, all others a neutral one. Never set `inp::input_focus` by hand
- **Edits go through `scene_document::commit()`**, which calls `serialize_and_push()` - destructive — tears down and rebuilds the entire ECS world; don't hold raw pointers to registry components across it
- **Camera state**: `scene_editor::on_save_camera` runs before the push and `on_inject_camera` after (state in `scene_document::camera`) — otherwise camera resets
- **Memory resources**: `memory://` protocol stores resources at runtime; `res://` resolves from filesystem via `resource.path`
- **`get_field_desc<T>` with inline objects** requires `__type` field present in the JSON object
- **`children` is a JSON array of named nodes (v2)** — iterate via `as_array()`; a v1 object is only valid before `migrate_tree`
- **No C functions** — prefer `std::format_to_n` over `snprintf`/`strncpy`, C++ alternatives over C stdlib
