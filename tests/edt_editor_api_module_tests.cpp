#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include "null_engine_fixture.hpp"
#include "edt_document_cast.h"
#include "edt_scene_document.h"
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <memory>
#include <string>

#if defined(SNK_TEST_EDITOR_API_DLL_PATH)

#if defined(_WIN32)
	#include <windows.h>
#else
	#include <dlfcn.h>
#endif

// A module loaded from disk registers a panel into the editor_context through the shared editor API library, and the
// registry is as before once it dropped the registration (#587). The module is built only with the shared engine.

namespace {

struct module_library
{
	explicit module_library(const std::string& path)
	{
#if defined(_WIN32)
		handle = ::LoadLibraryA(path.c_str());
#else
		handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
	}

	~module_library()
	{
		if (!handle) return;
#if defined(_WIN32)
		::FreeLibrary(static_cast<HMODULE>(handle));
#else
		::dlclose(handle);
#endif
	}

	template<typename Fn>
	Fn symbol(const char* name) const
	{
#if defined(_WIN32)
		return reinterpret_cast<Fn>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
		return reinterpret_cast<Fn>(::dlsym(handle, name));
#endif
	}

	void* handle = nullptr;
};

struct api_context_fixture : test_util::null_engine_fixture
{
	api_context_fixture()
		: null_engine_fixture(false)
		, gui(nullptr, nullptr)
		, services{ .ctx_manager = storage.require_shared<scn::context_manager>(),
			.base_descs = &storage.require<desc::desc_system>(), .base_resources = &storage.require<res::resource_system>(),
			.base_assembler = &storage.require<scn::ecs_assembler>(), .base_sfactory = &storage.require<ecs::system_factory>() }
		, context(std::make_unique<edt::editor_context>(
			storage.require<res::resource_system>(), storage.require<rnd::render_system>(),
			gui, services, tasks, commands, scene, edt::prefab_preview(services)))
	{
	}

	gui::gui_system gui;
	cmd::command_registry commands;
	edt::editor_services services;
	edt::scene_document scene;
	std::unique_ptr<edt::editor_context> context;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtEditorApiModuleTests, api_context_fixture)

BOOST_AUTO_TEST_CASE(AModule_RegistersAPanelThroughTheApi_AndLeavesNoTraceWhenItDropsIt)
{
	module_library library(SNK_TEST_EDITOR_API_DLL_PATH);
	BOOST_REQUIRE_MESSAGE(library.handle != nullptr, "the test module did not load");
	const auto register_probe = library.symbol<void (*)(edt::editor_context*)>("probe_register");
	const auto unregister_probe = library.symbol<void (*)()>("probe_unregister");
	BOOST_REQUIRE(register_probe != nullptr && unregister_probe != nullptr);

	const std::size_t before = context->panels().size();
	register_probe(context.get());
	BOOST_CHECK(context->panels().contains("probe.panel"));
	BOOST_CHECK_EQUAL(context->panels().size(), before + 1);

	unregister_probe();
	BOOST_CHECK(!context->panels().contains("probe.panel"));
	BOOST_CHECK_EQUAL(context->panels().size(), before);
}

BOOST_AUTO_TEST_SUITE_END()

#endif
