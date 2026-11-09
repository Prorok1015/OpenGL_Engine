#pragma once
#include <snk/application/app_module_interface.h>
#include <entt/config/config.h>
#include <cstdint>
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
#define SNK_MODULE_API_VERSION 0x0003'0004u // major << 16 | minor

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

}

// The module_entry of the four functions SNK_DECLARE_MODULE_PREFIXED(prefix, ...) defines.
#define SNK_MODULE_ENTRY(prefix) \
	::app::modules::module_entry{ &prefix##module_api_version, &prefix##module_build_info, &prefix##create_module, \
		&prefix##destroy_module }

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
