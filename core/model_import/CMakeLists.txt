cmake_minimum_required(VERSION 3.16)
project(model_import_assimp)

# snk_model_import_assimp (#1108): the one reading of a model file (assimp) into the engine's descs, and the only owner of
# assimp. The engine never links it: a game, the tools and the editor's decoder link it next to the engine. Added only
# when SNK_WITH_ASSIMP is ON (core/CMakeLists.txt). Shared next to snk_engine when the engine is shared (one assimp per
# process, on MSVC too), static otherwise.

# The ASSIMP_* options are set in cmake/ThirdParty.cmake, before this.
add_subdirectory(${THIRD_PARTY_PATH}/assimp assimp)

if(SNK_ENGINE_SHARED)
	set(_mdl_type SHARED)
else()
	set(_mdl_type STATIC)
endif()
add_library(snk_model_import_assimp ${_mdl_type}
	mdl_assimp_io.cpp mdl_assimp_io.h mdl_import.cpp mdl_model_adapter.cpp)
add_library(snk::model_import_assimp ALIAS snk_model_import_assimp)
# In the static SDK set: snk::static_model_import_assimp (snk::game_host links it).
set_property(TARGET snk_model_import_assimp PROPERTY EXPORT_NAME model_import_assimp)
# Compiled under core/ but not part of snk_engine: no SNK_ENGINE_BUILD (cmake/SnkTarget.cmake), SNK_API imports.
set_property(TARGET snk_model_import_assimp PROPERTY SNK_OUTSIDE_ENGINE TRUE)
snk_component_headers(snk_model_import_assimp)

# Like snk_editor_api: the engine's headers publicly (the module's headers name its types), its code from snk_engine.
target_link_libraries(snk_model_import_assimp PUBLIC snk::engine_api PRIVATE snk::engine PRIVATE assimp
	PRIVATE $<BUILD_LOCAL_INTERFACE:snk::sanitizers>)
if(SNK_ENGINE_SHARED)
	# SNK_MODEL_IMPORT_SHARED reaches every consumer, SNK_MODEL_IMPORT_BUILD only the library itself (mdl_api.h).
	target_compile_definitions(snk_model_import_assimp PUBLIC SNK_MODEL_IMPORT_SHARED PRIVATE SNK_MODEL_IMPORT_BUILD)
	if(MSVC)
		# snk_engine.dll exports only SNK_API: Boost.JSON is linked in as its own static copy (as editor_system does).
		target_link_libraries(snk_model_import_assimp PRIVATE Boost::json)
		target_compile_options(snk_model_import_assimp INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/wd4251> $<$<COMPILE_LANGUAGE:CXX>:/wd4275>)
	endif()

	# Next to editor.exe and snk_model_decoder (component `editor`): Windows next to the binary, Linux in lib/ (the
	# executables' RUNPATH is $ORIGIN/../lib; this library finds libsnk_engine.so beside itself). No SDK target links it
	# in a shared build, so neither its import library nor its headers are installed.
	install(TARGETS snk_model_import_assimp
		RUNTIME DESTINATION bin COMPONENT editor
		LIBRARY DESTINATION lib COMPONENT editor
		ARCHIVE DESTINATION lib/snk COMPONENT model_import_assimp_dev EXCLUDE_FROM_ALL)
	if(UNIX AND NOT APPLE)
		set_target_properties(snk_model_import_assimp PROPERTIES INSTALL_RPATH "$ORIGIN")
	endif()
	if(MSVC)
		install(FILES $<TARGET_PDB_FILE:snk_model_import_assimp> DESTINATION bin OPTIONAL COMPONENT editor)
	endif()
endif()

# snk_model_import_core (#1077): the same reading of a model (mdl_import.h) for the model decoder process, static and without
# the engine: the codecs from snk_model_codec, assimp linked in. Nothing but the decoder links it, so no binary holds it next
# to snk_engine. Compiled under core/ it gets SNK_ENGINE_BUILD in a shared build (cmake/SnkTarget.cmake), like the archives
# it is linked with: SNK_API is the export side everywhere in the decoder, never dllimport of a DLL it does not load.
add_library(snk_model_import_core STATIC mdl_assimp_io.cpp mdl_assimp_io.h mdl_import.cpp)
add_library(snk::model_import_core ALIAS snk_model_import_core)
target_include_directories(snk_model_import_core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
	PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# The decoder links no engine but reads declarations of resource, desc, render and scene (types and inline code only, the
# code it calls is in snk_model_codec): their include directories, not their libraries (#1161).
target_include_directories(snk_model_import_core PUBLIC
	"$<BUILD_INTERFACE:${ENGINE_CORE_PATH}/core/resource/include>"
	"$<BUILD_INTERFACE:${ENGINE_CORE_PATH}/core/desc/include>"
	"$<BUILD_INTERFACE:${ENGINE_CORE_PATH}/engine/render/render_system/include>"
	"$<BUILD_INTERFACE:${ENGINE_CORE_PATH}/engine/scene/include>")
target_link_libraries(snk_model_import_core PUBLIC snk::model_codec PUBLIC snk::common PRIVATE snk::engine_logger PRIVATE assimp
	PRIVATE $<BUILD_LOCAL_INTERFACE:snk::sanitizers>)
