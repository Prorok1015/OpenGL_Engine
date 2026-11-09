cmake_minimum_required(VERSION 3.16)
project(model_import)

# snk_model_import: reading a model file into the engine's descs (assimp) and what guards what a model may make the
# editor read (model_file_access, file_identity). It knows nothing of the editor: editor_system links it for the
# import of a person and of an agent, and the decoder process (#647) will link it too, so the same code runs on both
# sides of the process boundary.
add_library(${PROJECT_NAME} STATIC)
add_library(snk::model_import ALIAS ${PROJECT_NAME})

set(SE_MODEL_IMPORT_SOURCES edt_decode_bundle.cpp edt_decode_codec.cpp edt_file_identity.cpp edt_model_file_access.cpp)
if(SNK_WITH_ASSIMP)
	list(APPEND SE_MODEL_IMPORT_SOURCES edt_filesystem_assimp_io.cpp edt_model_importer.cpp)
endif()
list(TRANSFORM SE_MODEL_IMPORT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(${PROJECT_NAME} PRIVATE ${SE_MODEL_IMPORT_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

# As editor_system: the engine and its headers; assimp is usage-only in the shared build (the code lives in
# snk_engine), except on MSVC where a DLL exports only SNK_API and the consumer links its own static copy.
set(_model_import_assimp "")
if(SNK_WITH_ASSIMP)
	snk_usage_only(_model_import_assimp assimp)
endif()
if(SNK_WITH_ASSIMP AND SNK_ENGINE_SHARED AND MSVC)
	set(_model_import_assimp assimp Boost::json)
endif()
target_link_libraries(${PROJECT_NAME} PUBLIC snk::engine_api PRIVATE snk::engine PRIVATE ${_model_import_assimp})
if(SNK_ENGINE_SHARED AND MSVC)
	# The bundle reader parses JSON: snk_engine.dll does not export Boost.JSON.
	target_link_libraries(${PROJECT_NAME} PRIVATE Boost::json)
endif()

# The editor imports models through it (the sources moved here from editor_system/import).
target_link_libraries(editor_system PUBLIC ${PROJECT_NAME})
