#pragma once
#include <snk/application/app_module_interface.h>
#include <entt/config/config.h>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <version>

// C entry point of a loadable game module (EPIC-29 #62, docs/roadmap/design/EPIC-29_SERVICES_DLL.md §6).
//
// A module binary exports four C-named functions; behind them is a C++ module_interface, so host
// and module must share toolchain, standard library and CRT. The host checks that through
// snk_module_build_info before it creates anything, and the binary that created a module
// destroys it (snk_destroy_module), so allocation never crosses a heap boundary.

// 2.0 (#595): resource control blocks and res_dep, which inline engine code compiled into a module uses, changed layout.
// 3.0 (#603): the world bus is inline too (late-sender fields added); any change to the layout of ecs::detail::bus_state,
// bus_slot_impl or event_writer/event_reader is a major bump. Also changed in 3.0: the layouts of scn::context_set,
// context_manager and scene_layer (#603) and the fields #638 appended to world_context, world_desc and context_set.
// 3.1 (#603): gs::module_context::subscribe; a module that uses it needs an engine of 3.1 or newer.
// 3.2 (#604): game actions. New ctx types inp::action_state, inp::action_map_ref, inp::input_bound and the engine event
// inp::action_event; the bus and every existing type keep their layout, so a module built for 3.0 or 3.1 still loads.
// 3.3 (#604): inp::fixed_action_state, a new ctx type; nothing else changes layout. action_state::update and
// inp::update_actions_system changed signature: they are the engine's, a module reads the ctx types and never calls them.
// 3.4 (#600): gs::module_context::observe (component observers the hot reload in Play mutes). The mute depth is appended
// to the private, heap-held state of scn::event_subscriptions, so no layout a module reads changes; a module that uses
// observe needs an engine of 3.4 or newer.
// 3.5 (#624): inp::action_input, a new ctx type (what of an action reached the layers of a world that declare it).
// inp::stack_layer_desc gains `all_actions` at its end (the layer declares every action); nothing that exists moves.
// inp::action_input is read by the engine's inp::update_actions alone (no module, template or test DLL reads it); its
// element action_reach grew (armed_keys, armed_buttons: the memory of fronts, #624 PR b) before any module could have
// been built against it, so the minor stays 3.5. inp::action_injection (#624 PR c, the edges of inp::input_router::inject)
// is a ctx type of the same kind: gs::route_input writes it, inp::update_actions reads it, no module does.
// inp::world_input and inp::drag_hold are the engine's own (the result and an argument of inp::resolve), which also
// gained a parameter, like inp::update_actions_system and action_state::update: a module calls none of them.
// 3.6 (#614): the optional export snk_module_layout_key, the key of the layouts of the engine types whose size is compiled
// into module code (gs::module_layout_key, gs_module_layout.h). The host refuses a module whose key differs from its
// own before it creates it; a module without the export (3.5 and older) or with key 0 is checked by version only.
// Changing the size or alignment of a type of that list is a major bump: the key refuses only modules that have it.
// #631 (no bump, from 3.6 on): res::tag parses its protocol inline; a module built before #631 parses its own strings the old way until rebuilt.
// 3.7 (#625): the input stack from game code. gs::module_context::input_stack(set) and gs::input_stack_handle (exported
// methods; module_context is made by the engine and gains two members at its end), the ctx type inp::input_stack_control
// (a system's list of stack commands, methods exported), inp::stack_command and inp::stack_layer_desc::owner already there.
// A module built for 3.6 or older never calls them and still loads; one that uses them needs an engine of 3.7 or newer.
// gs::input_stack_handle, inp::stack_layer_desc and inp::input_stack_control are in the layout key (#614: the module holds,
// builds and allocates them), so the key changes; a module without the key export (3.6 and older) is checked by version only.
// 3.8 (#551): scn::spawn_prefab, scn::despawn and scn::bind_prefab_spawning, new functions a game module imports, and
// the components scn::world_root_component and scn::spawned_root_component; nothing that exists changes layout. A module
// that spawns prefabs needs an engine of 3.8 or newer.
// 3.9 (#697): desc_system::get_field_desc, inline in module code, refuses a reference to user://, cache://, code://, edt:// through
// the new export resource_system::refused_block; a module built for 3.9 needs an engine of 3.9. No layout changes.
// 3.10 (#564): scn::request_level and scn::level_switch, new functions a game module imports, and the enum and struct
// they use (scn::level_switch_status, scn::level_switch_state). scn::context_set gains a member at its end; the request
// itself is a private ctx type of the engine. Nothing a module reads moves. A module that switches levels needs an
// engine of 3.10 or newer.
// 3.11 (#606): components without a C++ type. scn::register_schema_component, scn::schema_component_registration,
// component_catalog::add_schema_component, scn::holds and the blob accessors are new exports; context_manager gains
// members at its end (engine-made, not in the layout key). A module that registers one needs an engine of 3.11 or newer.
// A module built before 3.11 that walks the catalog must not call entry.contains on such an entry (it asserts in debug
// and answers false in release): it learns of them only by being rebuilt, and then asks through scn::holds.
// 3.14 (#605): the bus journal (ecs::bus_journal, world_bus::enable_journal/journal, ecs::detail::bus_journal_source)
// and bus_registry::add<T>(schema, payload_writer) are new; bus_type_impl<T> gains a base, which only modules built for
// 3.14 have: the journal of a type of an older module holds the place of its events, not their sender or payload.
// bus_slot, bus_slot_impl and the layout key are unchanged; bus_state, input_router gain members at their end
// (engine-made). inp::action_trace and inp::update_and_trace_actions_system are new. A module that gives its events a
// payload writer needs an engine of 3.14 or newer.
#define SNK_MODULE_API_VERSION 0x0003'000Eu // major << 16 | minor

// The key SNK_DECLARE_MODULE exports: snk/game/gs_module_layout.h defines it, whatever the order of the includes;
// without that header a module reports 0, which the host does not check.
#ifndef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY 0u
#endif

#if defined(_WIN32)
#define SNK_MODULE_EXPORT __declspec(dllexport)
#else
#define SNK_MODULE_EXPORT __attribute__((visibility("default")))
#endif

struct snk_module_build_info
{
	std::uint32_t api;      // SNK_MODULE_API_VERSION the module was built against
	std::uint32_t compiler; // (compiler id << 16) | major version, see app::modules::compiler_id
	std::uint32_t flags;    // app::modules::build_flag bits
};

namespace app::modules {

	namespace build_flag {
		inline constexpr std::uint32_t debug = 1u << 0;       // debug build (debug CRT on MSVC, no NDEBUG elsewhere)
		inline constexpr std::uint32_t static_crt = 1u << 1;  // MSVC /MT or /MTd
		inline constexpr std::uint32_t libcxx = 1u << 2;      // LLVM libc++ (otherwise libstdc++ / MSVC STL)
		inline constexpr std::uint32_t old_stl_abi = 1u << 3; // libstdc++ _GLIBCXX_USE_CXX11_ABI=0
		// EnTT type ids and container layouts cross the boundary, so EnTT's mode is part of the ABI (#218).
		inline constexpr std::uint32_t entt_standard_cpp = 1u << 4; // ENTT_STANDARD_CPP
		inline constexpr std::uint32_t entt_wide_id = 1u << 5;      // ENTT_ID_TYPE other than 32 bits
		inline constexpr std::uint32_t entt_custom_page = 1u << 6;  // ENTT_SPARSE_PAGE / ENTT_PACKED_PAGE not the defaults
		// With entt_custom_page, log2 of ENTT_SPARSE_PAGE in bits 8..15 and of ENTT_PACKED_PAGE in bits 16..23: two
		// different custom sizes differ too.
		inline constexpr std::uint32_t entt_sparse_page_shift = 8;
		inline constexpr std::uint32_t entt_packed_page_shift = 16;
	}

	namespace compiler_id {
		inline constexpr std::uint32_t unknown = 0;
		inline constexpr std::uint32_t msvc = 1;
		inline constexpr std::uint32_t gcc = 2;
		inline constexpr std::uint32_t clang = 3;
	}

	constexpr std::uint32_t log2_of(std::uint64_t value) noexcept
	{
		std::uint32_t bits = 0;
		while (value > 1) {
			value >>= 1;
			++bits;
		}
		return bits;
	}

	constexpr std::uint32_t api_major(std::uint32_t version) noexcept { return version >> 16; }
	constexpr std::uint32_t api_minor(std::uint32_t version) noexcept { return version & 0xffffu; }

	// Evaluated from predefined macros, so it describes whichever binary compiles the call.
	constexpr snk_module_build_info make_build_info() noexcept
	{
		std::uint32_t compiler = 0;
		std::uint32_t flags = 0;
#if defined(__clang__)
		compiler = (compiler_id::clang << 16) | static_cast<std::uint32_t>(__clang_major__);
#elif defined(_MSC_VER)
		compiler = (compiler_id::msvc << 16) | static_cast<std::uint32_t>(_MSC_VER);
#elif defined(__GNUC__)
		compiler = (compiler_id::gcc << 16) | static_cast<std::uint32_t>(__GNUC__);
#endif

#if defined(_MSC_VER)
#if defined(_DEBUG)
		flags |= build_flag::debug;
#endif
#if !defined(_DLL)
		flags |= build_flag::static_crt;
#endif
#elif !defined(NDEBUG)
		flags |= build_flag::debug;
#endif
#if defined(_LIBCPP_VERSION)
		flags |= build_flag::libcxx;
#endif
#if defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 0
		flags |= build_flag::old_stl_abi;
#endif
#if defined(ENTT_STANDARD_CPP)
		flags |= build_flag::entt_standard_cpp;
#endif
		if constexpr (sizeof(ENTT_ID_TYPE) != sizeof(std::uint32_t)) {
			flags |= build_flag::entt_wide_id;
		}
		// The storages' page sizes: a registry built with others does not index the same pools the same way.
		if constexpr (ENTT_SPARSE_PAGE != 4096 || ENTT_PACKED_PAGE != 1024) {
			flags |= build_flag::entt_custom_page
				| (log2_of(ENTT_SPARSE_PAGE) << build_flag::entt_sparse_page_shift)
				| (log2_of(ENTT_PACKED_PAGE) << build_flag::entt_packed_page_shift);
		}
		return snk_module_build_info{ SNK_MODULE_API_VERSION, compiler, flags };
	}

	// Deletes through the creating binary's destroy function when there is one.
	struct module_deleter
	{
		void (*destroy)(module_interface*) = nullptr;

		void operator()(module_interface* m) const
		{
			if (destroy) {
				destroy(m);
			} else {
				delete m;
			}
		}
	};
	using module_ptr = std::unique_ptr<module_interface, module_deleter>;

	// The four exported functions of a module binary, resolved by a module_source (app_module_source.h).
	struct module_entry
	{
		std::uint32_t (*api_version)() = nullptr;
		const struct snk_module_build_info* (*build_info)() = nullptr;
		module_interface* (*create)() = nullptr;
		void (*destroy)(module_interface*) = nullptr;
		// Only an editor module (kind "editor", #588) has it: snk_editor_api_version, the editor API it was built against.
		// The host that loads editor modules checks it before the module is created; a runtime module has none.
		std::uint32_t (*editor_api_version)() = nullptr;
		// snk_module_layout_key (3.6, #614); null for a module built for 3.5 or older.
		std::uint64_t (*layout_key)() = nullptr;
	};

	// Empty string when the module can be loaded by this host, otherwise the reason it cannot.
	inline std::string check_module_entry(const module_entry& entry)
	{
		if (!entry.api_version || !entry.build_info || !entry.create || !entry.destroy) {
			return "entry point is incomplete (a snk_* function is missing)";
		}
		const std::uint32_t version = entry.api_version();
		const std::uint32_t host_version = SNK_MODULE_API_VERSION;
		if (api_major(version) != api_major(host_version)) {
			return "api major version " + std::to_string(api_major(version)) + " does not match host major "
				+ std::to_string(api_major(host_version));
		}
		if (api_minor(version) > api_minor(host_version)) {
			return "module needs api minor " + std::to_string(api_minor(version)) + ", host provides "
				+ std::to_string(api_minor(host_version));
		}

		const snk_module_build_info* info = entry.build_info();
		if (!info) {
			return "module returned no build info";
		}
		if (info->api != version) {
			return "build info api does not match snk_module_api_version";
		}
		const snk_module_build_info host = make_build_info();
		// Same compiler family; for MSVC every 19.x toolset is binary compatible, otherwise the major must match.
		const bool same_compiler = (info->compiler >> 16) == (host.compiler >> 16)
			&& ((host.compiler >> 16) == compiler_id::msvc || info->compiler == host.compiler);
		if (!same_compiler) {
			return "built with a different compiler (module " + std::to_string(info->compiler)
				+ ", host " + std::to_string(host.compiler) + ")";
		}
		if (info->flags != host.flags) {
			return "build flags differ (module " + std::to_string(info->flags) + ", host "
				+ std::to_string(host.flags) + "): debug/release, CRT, standard library or EnTT mode mismatch";
		}
		return {};
	}

	// Empty when the module was built against the engine types' layouts the host has (`host_key`,
	// gs::engine_module_layout_key), or cannot tell (no snk_module_layout_key, key 0, `host_key` 0); otherwise why not.
	inline std::string check_module_layout(const module_entry& entry, std::uint64_t host_key)
	{
		if (!entry.layout_key || host_key == 0) {
			return {};
		}
		const std::uint64_t key = entry.layout_key();
		if (key == 0 || key == host_key) {
			return {};
		}
		return std::format("it was built against an engine whose types have another layout (module key {:016x}, engine "
			"key {:016x}): rebuild the project (Build > Build Game)", key, host_key);
	}

}

// The module_entry of the functions SNK_DECLARE_MODULE_PREFIXED(prefix, ...) defines.
#define SNK_MODULE_ENTRY(prefix) \
	::app::modules::module_entry{ &prefix##module_api_version, &prefix##module_build_info, &prefix##create_module, \
		&prefix##destroy_module, nullptr, &prefix##module_layout_key }

// The game module compiled into a static host (snk_add_game_host, #592): the host gets its entry from here, not from a
// library it opens. Defined by SNK_DECLARE_MODULE when the module is built with SNK_GAME_MODULE_STATIC.
app::modules::module_entry snk_static_module_entry();

#define SNK_DETAIL_NO_EXPORT

#define SNK_DETAIL_DECLARE_MODULE_FUNCTIONS(attr, prefix, Type) \
	extern "C" { \
	attr std::uint32_t prefix##module_api_version() \
	{ \
		return SNK_MODULE_API_VERSION; \
	} \
	attr const struct snk_module_build_info* prefix##module_build_info() \
	{ \
		static const struct snk_module_build_info info = ::app::modules::make_build_info(); \
		return &info; \
	} \
	attr ::app::modules::module_interface* prefix##create_module() \
	{ \
		return new Type(); \
	} \
	attr void prefix##destroy_module(::app::modules::module_interface* module) \
	{ \
		delete module; \
	} \
	attr std::uint64_t prefix##module_layout_key() \
	{ \
		return SNK_DETAIL_MODULE_LAYOUT_KEY; \
	} \
	}

// Generates the four exported functions of a module binary under the given prefix:
// <prefix>module_api_version, <prefix>module_build_info, <prefix>create_module, <prefix>destroy_module.
// Use SNK_DECLARE_MODULE in a module binary; the prefixed form exists so one binary (a unit test) can
// hold several declarations without clashing C symbols.
#define SNK_DECLARE_MODULE_PREFIXED(prefix, Type) SNK_DETAIL_DECLARE_MODULE_FUNCTIONS(SNK_MODULE_EXPORT, prefix, Type)

#if defined(SNK_GAME_MODULE_STATIC)
// Linked into the host executable: nothing is exported, and the host takes the entry from snk_static_module_entry().
#define SNK_DECLARE_MODULE(Type) \
	SNK_DETAIL_DECLARE_MODULE_FUNCTIONS(SNK_DETAIL_NO_EXPORT, snk_, Type) \
	::app::modules::module_entry snk_static_module_entry() \
	{ \
		return SNK_MODULE_ENTRY(snk_); \
	}
#else
#define SNK_DECLARE_MODULE(Type) SNK_DECLARE_MODULE_PREFIXED(snk_, Type)
#endif
