#include <boost/test/unit_test.hpp>
#include "../test_scene_env.hpp"
#include <snk/game/gs_input_actions.h>
#include <snk/input/inp_actions.h>
#include <snk/input/inp_events.hpp>
#include <filesystem>
#include <string>

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

// The player's bindings across the module boundary (US-542-C): a module of test_input_actions_dll.cpp reads the action map
// inline and calls the exports of gs::input_actions, the old three-argument rebind of 3.16 included.

namespace {

using KB = inp::KEYBOARD_BUTTONS;

constexpr const char* ACTIONS = R"json({"__type":"input_actions_desc","__version":1,"actions":[
	{"name":"Jump","bindings":[{"key":"SPACE"}]},{"name":"Fire","bindings":[{"key":"F"}]}]})json";

constexpr int APPLIED = static_cast<int>(inp::rebind_status::applied);
constexpr int CONFLICT = static_cast<int>(inp::rebind_status::conflict);
constexpr int BAD_SLOT = static_cast<int>(inp::rebind_status::bad_slot);

// The library, loaded from disk and closed with the test.
struct input_module
{
	using key_at_fn = int (*)(const inp::action_map*, const char*, int);
	using count_fn = int (*)(const inp::action_map*, const char*);
	using rebind_fn = int (*)(gs::input_actions*, const char*, int, int, int);
	using remove_fn = int (*)(gs::input_actions*, const char*, int);

	input_module()
	{
#if defined(_WIN32)
		handle = ::LoadLibraryW(std::filesystem::path(SNK_TEST_INPUT_ACTIONS_DLL_PATH).c_str());
		BOOST_REQUIRE(handle != nullptr);
		const auto symbol = [this](const char* name) { return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name)); };
#else
		handle = ::dlopen(SNK_TEST_INPUT_ACTIONS_DLL_PATH, RTLD_NOW | RTLD_LOCAL);
		BOOST_REQUIRE_MESSAGE(handle != nullptr, ::dlerror());
		const auto symbol = [this](const char* name) { return ::dlsym(handle, name); };
#endif
		key_at = reinterpret_cast<key_at_fn>(symbol("snk_test_key_at"));
		count = reinterpret_cast<count_fn>(symbol("snk_test_binding_count"));
		rebind = reinterpret_cast<rebind_fn>(symbol("snk_test_rebind"));
		remove = reinterpret_cast<remove_fn>(symbol("snk_test_remove"));
		BOOST_REQUIRE(key_at && count && rebind && remove);
	}

	~input_module()
	{
		if (!handle) return;
#if defined(_WIN32)
		::FreeLibrary(static_cast<HMODULE>(handle));
#else
		::dlclose(handle);
#endif
	}

	void* handle = nullptr;
	key_at_fn key_at = nullptr;
	count_fn count = nullptr;
	rebind_fn rebind = nullptr;
	remove_fn remove = nullptr;
};

struct input_dll_fixture : test_util::scene_env
{
	// The desc loads on the task pump between two updates.
	void load(gs::input_actions& actions, const res::tag& tag)
	{
		actions.update(tag.view());
		pump();
		actions.update(tag.view());
	}
};

}

BOOST_FIXTURE_TEST_SUITE(GsInputActionsDllTests, input_dll_fixture)

BOOST_AUTO_TEST_CASE(AModuleReadsTheMapInlineAndRebindsThroughTheExports)
{
	input_module module;
	gs::input_actions actions(*rs);
	load(actions, put("actions.desc", ACTIONS));
	BOOST_REQUIRE(actions.map());

	BOOST_TEST(module.key_at(actions.map().get(), "Jump", 0) == static_cast<int>(KB::SPACE));
	BOOST_TEST(module.key_at(actions.map().get(), "Fire", 0) == static_cast<int>(KB::F));
	BOOST_TEST(module.key_at(actions.map().get(), "Nope", 0) == -3);

	// The three-argument rebind: the symbol of 3.16, a conflict is refused.
	BOOST_TEST(module.rebind(&actions, "Jump", 0, static_cast<int>(KB::K), -1) == APPLIED);
	BOOST_TEST(module.key_at(actions.map().get(), "Jump", 0) == static_cast<int>(KB::K));
	BOOST_TEST(module.rebind(&actions, "Fire", 0, static_cast<int>(KB::K), -1) == CONFLICT);
	BOOST_TEST(module.key_at(actions.map().get(), "Fire", 0) == static_cast<int>(KB::F));
	BOOST_TEST(actions.conflicts().empty());

	// The four-argument one with the conflict allowed: both keep it.
	BOOST_TEST(module.rebind(&actions, "Fire", 0, static_cast<int>(KB::K), 1) == APPLIED);
	BOOST_TEST(module.key_at(actions.map().get(), "Fire", 0) == static_cast<int>(KB::K));
	BOOST_TEST(actions.conflicts().size() == 1u);

	BOOST_TEST(module.remove(&actions, "Fire", 0) == APPLIED);
	BOOST_TEST(module.count(actions.map().get(), "Fire") == 0);
	BOOST_TEST(module.remove(&actions, "Fire", 0) == BAD_SLOT);
	BOOST_TEST(actions.conflicts().empty());
}

BOOST_AUTO_TEST_SUITE_END()
