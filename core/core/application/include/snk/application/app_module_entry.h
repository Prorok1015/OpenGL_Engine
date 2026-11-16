#pragma once
#include <snk/application/app_module_interface.h>
#include <entt/config/config.h>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <version>

// C entry point of a loadable game module (EPIC-29 #62, docs/roadmap/design/EPIC-29_SERVICES_DLL.md sec. 6).
//
// A module binary exports four C-named functions; behind them is a C++ module_interface, so host
// and module must share toolchain, standard library and CRT. The host checks that through
// snk_module_build_info before it creates anything, and the binary that created a module
// destroys it (snk_destroy_module), so allocation never crosses a heap boundary.

// Version: a major bump refuses every module built before; a minor one is a new export or a type the engine alone
// makes. From 4.0 (#758) layout changes go through gs::detail::layout_revision (gs_module_layout.h) instead, until the
// window of 4.0 closes. What each change did is in docs/abi/HISTORY.md (up to #1056) and docs/abi/changes/<issue>.md
// (one file per change, so two changes never touch one line; tools/ci/check_abi_changes.py asks for it).
#define SNK_MODULE_API_VERSION 0x0004'0000u // major << 16 | minor

// The key SNK_DECLARE_MODULE exports: snk/game/gs_module_layout.h defines it, whatever the order of the includes;
// without that header a module reports 0, which the host refuses of a module in a library (check_library_module).
// With it the file that declares the module needs the key's type list, snk/game/gs_module_layout_key.h, or it does not
// compile: gs_game_module.h included after this header and edt_editor_extension.h bring it.
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

	// Empty when a module built by `module_compiler` (snk_module_build_info::compiler) can run in a host built by
	// `host_compiler`, otherwise why not. MSVC toolsets are binary compatible forwards only (#219): a module needs the
	// host's _MSC_VER or a newer one. GCC and Clang need the same major.
	inline std::string compiler_problem(std::uint32_t module_compiler, std::uint32_t host_compiler)
	{
		const bool both_msvc = (module_compiler >> 16) == compiler_id::msvc && (host_compiler >> 16) == compiler_id::msvc;
		if (both_msvc) {
			const std::uint32_t module_version = module_compiler & 0xffffu;
			const std::uint32_t host_version = host_compiler & 0xffffu;
			if (module_version < host_version) {
				return std::format("built with MSVC {} (_MSC_VER), older than the MSVC {} the engine was built with: rebuild "
					"the module with Visual Studio 2022 17.14 or newer, MSVC {} at least", module_version, host_version, host_version);
			}
			return {};
		}
		if (module_compiler != host_compiler) {
			return std::format("built with a different compiler (module {}, host {})", module_compiler, host_compiler);
		}
		return {};
	}

	// Empty string when the module can be loaded by this host, otherwise the reason it cannot.
	inline std::string check_module_entry(const module_entry& entry)
	{
		if (!entry.api_version || !entry.build_info || !entry.create || !entry.destroy) {
			return "entry point is incomplete (a snk_* function is missing)";
		}
		const std::uint32_t version = entry.api_version();
		const std::uint32_t host_version = SNK_MODULE_API_VERSION;
		if (api_major(version) != api_major(host_version)) {
			std::string problem = "api major version " + std::to_string(api_major(version)) + " does not match host major "
				+ std::to_string(api_major(host_version));
			if (api_major(version) < api_major(host_version)) {
				problem += ": the module was built against an older engine, rebuild the project (Build > Build Game)";
			}
			return problem;
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
		if (std::string problem = compiler_problem(info->compiler, host.compiler); !problem.empty()) {
			return problem;
		}
		if (info->flags != host.flags) {
			return "build flags differ (module " + std::to_string(info->flags) + ", host "
				+ std::to_string(host.flags) + "): debug/release, CRT, standard library or EnTT mode mismatch";
		}
		return {};
	}

	// Empty when the module was built against the engine types' layouts the host has (`host_key`,
	// gs::engine_module_layout_key), or cannot tell (no snk_module_layout_key, key 0, `host_key` 0); otherwise why not.
	// A module in a library without a key is refused before this, by check_library_module.
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

	// Why a library module is refused besides check_module_entry (#922), or empty: a static C runtime is a heap of its
	// own that engine objects would cross, and without a key nothing tells its engine types' layouts from ours.
	inline std::string library_module_problem(std::uint32_t flags, std::uint64_t layout_key)
	{
		if ((flags & build_flag::static_crt) != 0) {
			return "it was built with the static C runtime (/MT or /MTd): a module in a library must use the DLL runtime "
				"(/MD or /MDd) to share the engine's heap; rebuild it with CMAKE_MSVC_RUNTIME_LIBRARY "
				"MultiThreaded$<$<CONFIG:Debug>:Debug>DLL";
		}
		if (layout_key == 0) {
			return "it reports no layout key (snk_module_layout_key missing or 0): declare it with SNK_DECLARE_MODULE in a file "
				"that includes snk/game/gs_game_module.h and rebuild the project (Build > Build Game)";
		}
		return {};
	}

	// library_module_problem of an entry. An entry check_module_entry refuses passes: the loader then reports that
	// problem, which says what to change (another major, debug/release).
	inline std::string check_library_module(const module_entry& entry)
	{
		if (!check_module_entry(entry).empty()) {
			return {};
		}
		return library_module_problem(entry.build_info()->flags, entry.layout_key ? entry.layout_key() : 0u);
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
