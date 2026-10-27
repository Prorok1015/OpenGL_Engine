#include <boost/test/unit_test.hpp>
#include "../test_scene_env.hpp"
#include "spike_game_api.h"
#include <snk/application/app_module_entry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_type_id.hpp>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_system.h>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_model.h>
#include <entt/entt.hpp>
#include <bit>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Spike #218: a game module built as its own shared library (tests/module_dll/spike_game_module.cpp)
// is loaded from disk, registers a component, a desc type with its spawner and a system with the
// engine, runs, and is unloaded again. Built only with SNK_ENGINE_SHARED (the module links snk_engine).
// Findings: docs/roadmap/design/SPIKE-218-entt-dll.md.

namespace {

class shared_library
{
public:
	explicit shared_library(std::filesystem::path path)
		: m_path(std::move(path))
	{
#if defined(_WIN32)
		m_handle = ::LoadLibraryW(m_path.c_str());
#else
		m_handle = ::dlopen(m_path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (m_handle) {
			Dl_info info{};
			if (::dladdr(::dlsym(m_handle, "snk_module_api_version"), &info) != 0) {
				m_base = info.dli_fbase;
			}
		}
#endif
	}

	~shared_library() { unload(); }

	shared_library(const shared_library&) = delete;
	shared_library& operator=(const shared_library&) = delete;

	bool loaded() const { return m_handle != nullptr; }

	std::string error() const
	{
#if defined(_WIN32)
		return "GetLastError " + std::to_string(::GetLastError());
#else
		const char* text = ::dlerror();
		return text ? text : "";
#endif
	}

	template<class F>
	F symbol(const char* name) const
	{
#if defined(_WIN32)
		return std::bit_cast<F>(::GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
		return std::bit_cast<F>(::dlsym(m_handle, name));
#endif
	}

	// True when `address` (code or data) lies in this library's image.
	bool contains(const void* address) const
	{
		if (!m_handle || !address) {
			return false;
		}
#if defined(_WIN32)
		HMODULE owner = nullptr;
		const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
		return ::GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &owner) != 0 && owner == static_cast<HMODULE>(m_handle);
#else
		Dl_info info{};
		return ::dladdr(address, &info) != 0 && info.dli_fbase == m_base;
#endif
	}

	bool unload()
	{
		if (!m_handle) {
			return true;
		}
#if defined(_WIN32)
		const bool ok = ::FreeLibrary(static_cast<HMODULE>(m_handle)) != 0;
#else
		const bool ok = ::dlclose(m_handle) == 0;
#endif
		m_handle = nullptr;
		m_base = nullptr;
		return ok;
	}

	// After unload(): the image is still mapped (another reference, or GNU_UNIQUE symbols pin it on Linux).
	bool resident() const
	{
#if defined(_WIN32)
		return ::GetModuleHandleW(m_path.c_str()) != nullptr;
#else
		void* again = ::dlopen(m_path.c_str(), RTLD_NOW | RTLD_NOLOAD);
		if (again) {
			::dlclose(again);
		}
		return again != nullptr;
#endif
	}

private:
	std::filesystem::path m_path;
	void* m_handle = nullptr;
	const void* m_base = nullptr;
};

// The Itanium and MSVC ABIs keep the vptr of a single-inheritance polymorphic object at offset 0;
// EnTT storages (sigh_mixin -> basic_storage -> basic_sparse_set) are such objects.
const void* vtable_of(const void* object)
{
	const void* vptr = nullptr;
	std::memcpy(&vptr, object, sizeof(vptr));
	return vptr;
}

float g_host_seen_dx = 0.f;

// Host-side system over the game's component: the engine side reads what the module created.
void host_reads_velocity(entt::view<entt::get_t<const spike::velocity_component>> view)
{
	g_host_seen_dx = 0.f;
	for (auto&& [entity, velocity] : view.each()) {
		g_host_seen_dx += velocity.dx;
	}
}

constexpr entt::id_type velocity_hash = entt::type_hash<spike::velocity_component>::value();
constexpr entt::id_type module_only_hash = entt::type_hash<spike::module_only_component>::value();

struct module_dll_fixture
{
	module_dll_fixture()
	{
		reg.ctx().emplace<ecs::entity_spawner>();
		load();
	}

	~module_dll_fixture() { unload(); }

	module_dll_fixture(const module_dll_fixture&) = delete;
	module_dll_fixture& operator=(const module_dll_fixture&) = delete;

	void load(bool share_meta = true)
	{
		env.emplace();
		systems.emplace();
		meta_before = meta_ids();
		lib.emplace(std::filesystem::path{ SNK_SPIKE_GAME_PATH });
		BOOST_REQUIRE_MESSAGE(lib->loaded(), "cannot load " << SNK_SPIKE_GAME_PATH << ": " << lib->error());

		entry.api_version = lib->symbol<decltype(entry.api_version)>("snk_module_api_version");
		entry.build_info = lib->symbol<decltype(entry.build_info)>("snk_module_build_info");
		entry.create = lib->symbol<decltype(entry.create)>("snk_create_module");
		entry.destroy = lib->symbol<decltype(entry.destroy)>("snk_destroy_module");
		BOOST_REQUIRE_EQUAL(app::modules::check_module_entry(entry), std::string{});

		module = app::modules::module_ptr{ entry.create(), app::modules::module_deleter{ entry.destroy } };
		BOOST_REQUIRE(module);
		data.construct<spike::host_services>(*env->descs, *env->assembler, *systems, share_meta);
		module->register_services(data);
		module->initialize_services(data);
	}

	// Detach order: data whose code lives in the module goes first (storages, registrations, meta),
	// then the module object, then the image.
	void unload()
	{
		if (!lib) {
			return;
		}
		detached_storages = detach_module_storages(reg);
		if (auto* sandbox = reg.ctx().find<ecs::entity_spawner>()) {
			detached_storages += detach_module_storages(*sandbox);
		}
		if (module) {
			module->shutdown_services(data);
		}
		registrations_left = env->descs->is_registered(spike::velocity_desc_type) || env->assembler->has_spawner(spike::velocity_desc_type);
		meta_nodes_after_shutdown = module_meta_nodes();
		reset_module_meta();
		meta_nodes_left = module_meta_nodes();
		if (data.has_value<spike::host_services>()) {
			data.destruct<spike::host_services>();
		}
		systems.reset();
		module.reset();
		env.reset();
		unloaded_cleanly = lib->unload();
		resident_after_unload = lib->resident();
		lib.reset();
	}

	template<class F>
	F symbol(const char* name) const
	{
		const F fn = lib->symbol<F>(name);
		BOOST_REQUIRE_MESSAGE(fn != nullptr, "missing export " << name);
		return fn;
	}

	// The pools whose code (vtable) lives in the module: they would dangle after the unload.
	template<class Registry>
	std::size_t detach_module_storages(Registry& r)
	{
		std::vector<entt::id_type> owned;
		for (auto&& [id, pool] : r.storage()) {
			if (lib->contains(vtable_of(&pool))) {
				owned.push_back(id);
			}
		}
		for (const auto id : owned) {
			r.reset(id);
		}
		return owned.size();
	}

	static std::set<entt::id_type> meta_ids()
	{
		std::set<entt::id_type> ids;
		for (auto&& [key, type] : entt::resolve(ecs::meta_context())) {
			ids.insert(type.id());
		}
		return ids;
	}

	std::size_t module_meta_nodes() const
	{
		std::size_t count = 0;
		for (auto&& [key, type] : entt::resolve(ecs::meta_context())) {
			if (lib->contains(&type.info())) {
				++count;
			}
		}
		return count;
	}

	// Every meta type that appeared in the engine's context while the module was loaded, explicit
	// registrations and nodes created implicitly by entt::resolve<T>() in module code alike.
	void reset_module_meta()
	{
		for (const auto id : meta_ids()) {
			if (!meta_before.contains(id)) {
				entt::meta_reset(ecs::meta_context(), id);
			}
		}
	}

	entt::registry reg; // the world outlives the module
	std::optional<test_util::scene_env> env;
	std::optional<ecs::system_factory> systems;
	ds::app_data_storage data;
	std::optional<shared_library> lib;
	app::modules::module_entry entry;
	app::modules::module_ptr module;
	std::set<entt::id_type> meta_before;

	std::size_t detached_storages = 0;
	bool registrations_left = true;
	std::size_t meta_nodes_after_shutdown = 0;
	std::size_t meta_nodes_left = 0;
	bool unloaded_cleanly = false;
	bool resident_after_unload = true;
};

}

BOOST_AUTO_TEST_SUITE(app_module_dll_tests)

BOOST_FIXTURE_TEST_CASE(type_hashes_match_but_type_index_and_meta_context_are_per_binary, module_dll_fixture)
{
	spike::module_probe probe{};
	symbol<spike_probe_fn>("spike_probe")(&probe);

	BOOST_TEST(!probe.standard_cpp);
	BOOST_TEST(probe.velocity_hash == velocity_hash);
	BOOST_TEST(probe.engine_hash == entt::type_hash<scn::local_transform>::value());
	BOOST_TEST(probe.velocity_name == entt::type_name<spike::velocity_component>::value());
	BOOST_TEST(probe.desc_system_key == ds::type_id::value<desc::desc_system>());
	BOOST_TEST_MESSAGE("type_index velocity: module " << probe.velocity_index << ", host " << entt::type_index<spike::velocity_component>::value()
		<< "; local_transform: module " << probe.engine_index << ", host " << entt::type_index<scn::local_transform>::value());

	// entt::type_id<T>() is a function-local static: one instance per binary.
	BOOST_TEST(lib->contains(probe.velocity_info));
	BOOST_TEST(!lib->contains(&entt::type_id<spike::velocity_component>()));

	// Every binary has its own entt::locator<meta_ctx>, the test executable as well as the module.
	BOOST_TEST(&entt::locator<entt::meta_ctx>::value_or() != &ecs::meta_context());
	BOOST_TEST(probe.meta_ctx == &ecs::meta_context()); // the module attached in register_services
}

BOOST_FIXTURE_TEST_CASE(meta_in_the_module_context_is_invisible_to_the_engine, module_dll_fixture)
{
	unload();
	load(false);
	spike::module_probe probe{};
	symbol<spike_probe_fn>("spike_probe")(&probe);
	BOOST_TEST(probe.meta_ctx != &ecs::meta_context());

	symbol<spike_spawn_in_sandbox_fn>("spike_spawn_in_sandbox")(&reg, 1.5f);
	scn::world_context::apply_commands(reg);

	BOOST_TEST(!entt::resolve(ecs::meta_context(), velocity_hash));
	BOOST_TEST(!reg.storage(velocity_hash)); // the entity came over, its component did not
}

BOOST_FIXTURE_TEST_CASE(meta_in_the_engine_context_moves_sandbox_components, module_dll_fixture)
{
	symbol<spike_spawn_in_sandbox_fn>("spike_spawn_in_sandbox")(&reg, 1.5f);
	scn::world_context::apply_commands(reg);

	const auto type = entt::resolve(ecs::meta_context(), velocity_hash);
	BOOST_REQUIRE(type);
	BOOST_TEST(static_cast<bool>(type.func(ecs::loader_get_h)));
	BOOST_TEST(lib->contains(&type.info()));

	const auto* pool = reg.storage(velocity_hash);
	BOOST_REQUIRE(pool != nullptr);
	BOOST_TEST(pool->size() == 1u);
	BOOST_TEST(lib->contains(vtable_of(pool))); // created by meta_loader_get<T>, i.e. module code
	for (auto&& [entity, velocity] : reg.view<const spike::velocity_component>().each()) {
		BOOST_TEST(velocity.dx == 1.5f);
	}
}

BOOST_FIXTURE_TEST_CASE(assembler_spawns_module_component_and_module_system_moves_engine_component, module_dll_fixture)
{
	BOOST_TEST(env->descs->is_registered(spike::velocity_desc_type));
	BOOST_TEST(env->assembler->has_spawner(spike::velocity_desc_type));

	const auto tag = env->put("spike.prefab.desc", R"({
		"__type": "prefab_desc", "__version": 2,
		"components": { "velocity": { "__type": "spike_velocity_desc", "dx": 2.5 } }
	})");
	auto root = entt::entity{ entt::null };
	{
		auto prefab = env->load<scn::prefab_desc>(tag);
		BOOST_REQUIRE(prefab);
		root = reg.create();
		scn::assemble_prefab(*env->assembler, reg, root, *prefab, "spike_root", tag);
	}
	BOOST_REQUIRE(reg.all_of<spike::velocity_component>(root));
	BOOST_REQUIRE(reg.all_of<scn::local_transform>(root));
	BOOST_TEST(reg.get<spike::velocity_component>(root).dx == 2.5f);
	BOOST_TEST(lib->contains(vtable_of(reg.storage(velocity_hash))));
	const float x0 = reg.get<scn::local_transform>(root).local[3][0];

	systems->register_system<&host_reads_velocity>("spike_test::host_reads_velocity", { .phase = ecs::phase::late_update });
	const std::vector<std::string> names{ std::string{ spike::move_system_name }, "spike_test::host_reads_velocity" };
	{
		const auto plan = ecs::build_system_plan(systems->registry(), names, { .validate_access = false });
		BOOST_TEST(!plan.has_errors());
		BOOST_TEST(plan.system_count() == 2u);

		ecs::runtime_context ctx{ .current_registry = reg };
		for (const auto& phase : plan.phases) {
			for (const auto& node : phase.nodes) {
				node(ctx);
			}
		}
	}
	BOOST_TEST(reg.get<scn::local_transform>(root).local[3][0] == x0 + 2.5f);
	BOOST_TEST(g_host_seen_dx == 2.5f);
}

BOOST_FIXTURE_TEST_CASE(unload_detaches_module_data_and_reload_registers_again, module_dll_fixture)
{
	for (int cycle = 0; cycle < 2; ++cycle) {
		BOOST_TEST_CONTEXT("cycle " << cycle) {
			if (cycle > 0) {
				load();
			}
			symbol<spike_spawn_in_sandbox_fn>("spike_spawn_in_sandbox")(&reg, 1.f);
			scn::world_context::apply_commands(reg);

			const auto keeper = reg.create();
			reg.emplace<scn::depth_level>(keeper, 7u);
			symbol<spike_emplace_module_only_fn>("spike_emplace_module_only")(&reg, keeper, 42);
			BOOST_TEST(reg.get<spike::module_only_component>(keeper).value == 42);

			// An engine type resolved from module code gets its meta node from the module (type_info, functions).
			const void* implicit_info = symbol<spike_resolve_engine_type_fn>("spike_resolve_engine_type")();
			BOOST_TEST(lib->contains(implicit_info));

			unload();
			BOOST_TEST_MESSAGE("cycle " << cycle << ": module storages detached " << detached_storages
				<< ", module meta nodes after shutdown_services " << meta_nodes_after_shutdown
				<< ", image still mapped after unload " << resident_after_unload);
			BOOST_TEST(detached_storages >= 3u); // velocity in the world and in the sandbox, module_only
			BOOST_TEST(!registrations_left);
			BOOST_TEST(meta_nodes_after_shutdown >= 1u); // the implicit node outlives the module's own meta_reset
			BOOST_TEST(meta_nodes_left == 0u);
			BOOST_TEST(unloaded_cleanly);

			// The world outlives the module: engine data stays, module data is gone.
			BOOST_TEST(reg.valid(keeper));
			BOOST_TEST(reg.get<scn::depth_level>(keeper).value == 7u);
			BOOST_TEST(!reg.storage(velocity_hash));
			BOOST_TEST(!reg.storage(module_only_hash));
			BOOST_TEST(!entt::resolve(ecs::meta_context(), velocity_hash));
			reg.destroy(keeper);
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
