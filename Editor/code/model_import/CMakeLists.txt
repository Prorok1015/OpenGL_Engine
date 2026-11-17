cmake_minimum_required(VERSION 3.16)
project(model_import)

# snk_model_import: reading a model file into the engine's descs (through snk::model_import_assimp) and what guards
# what a model may make the editor read (model_file_access, file_identity). It knows nothing of the editor: editor_system links it for the
# import of a person and of an agent, and the decoder process (#647) links the part it runs (snk_model_decoder_side below).
add_library(${PROJECT_NAME} STATIC)
add_library(snk::model_import ALIAS ${PROJECT_NAME})

# What the decoder process runs (#1077): the bundle and its JSON, the guards of what a model reads and the import into an
# mdl::import_sink, nothing that needs a desc or resource system.
set(SE_MODEL_DECODER_SIDE_SOURCES edt_decoder_sandbox.cpp edt_decode_bundle.cpp edt_decode_codec.cpp edt_model_file_access.cpp)
if(SNK_WITH_ASSIMP)
	list(APPEND SE_MODEL_DECODER_SIDE_SOURCES edt_model_file_source.cpp edt_model_import_core.cpp)
endif()
list(TRANSFORM SE_MODEL_DECODER_SIDE_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

set(SE_MODEL_IMPORT_SOURCES edt_decoder_copy.cpp edt_decode_runner.cpp edt_process_decoder.cpp edt_person_model_import.cpp)
if(SNK_WITH_ASSIMP)
	list(APPEND SE_MODEL_IMPORT_SOURCES edt_model_importer.cpp edt_decoded_model_adapter.cpp)
endif()
list(TRANSFORM SE_MODEL_IMPORT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(${PROJECT_NAME} PRIVATE ${SE_MODEL_DECODER_SIDE_SOURCES} ${SE_MODEL_IMPORT_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

# As editor_system: the engine and its headers. The model is read by the model import library (#1108), which owns
# assimp; nothing here includes an assimp header.
target_link_libraries(${PROJECT_NAME} PUBLIC snk::engine_api PUBLIC snk::file_identity PRIVATE snk::engine)
if(TARGET snk::model_import_assimp)
	# PUBLIC: edt_model_file_source.h implements its file_source.
	target_link_libraries(${PROJECT_NAME} PUBLIC snk::model_import_assimp)
endif()
if(SNK_ENGINE_SHARED AND MSVC)
	# The bundle reader parses JSON: snk_engine.dll does not export Boost.JSON.
	target_link_libraries(${PROJECT_NAME} PRIVATE Boost::json)
endif()

# The editor imports models through it (the sources moved here from editor_system/import).
target_link_libraries(editor_system PUBLIC ${PROJECT_NAME})

# snk_model_decoder_side (#1077): the same decoder-side sources, compiled again for the decoder process, which links no
# engine: the import core of core/model_import (snk_model_import_core) and the archives snk_engine carries (snk_model_codec,
# common, the logger, platform), statically. In a shared build those archives are compiled as the engine's export side
# (SNK_ENGINE_BUILD: SNK_API = dllexport, cmake/SnkTarget.cmake); so is this, never dllimport of an snk_engine.dll the
# decoder does not load. No binary links it next to snk_engine (snk_check_engine_copies).
if(SNK_WITH_ASSIMP)
	add_library(snk_model_decoder_side STATIC ${SE_MODEL_DECODER_SIDE_SOURCES})
	add_library(snk::model_decoder_side ALIAS snk_model_decoder_side)
	target_include_directories(snk_model_decoder_side PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
	target_link_libraries(snk_model_decoder_side PUBLIC snk::model_import_core PUBLIC snk::model_codec PUBLIC snk::common
		PUBLIC snk::platform PUBLIC snk::file_identity PRIVATE snk::engine_logger PRIVATE $<BUILD_LOCAL_INTERFACE:snk::sanitizers>)
	if(SNK_ENGINE_SHARED)
		# PUBLIC on purpose: the decoder's own sources include the same headers and must not see SNK_API as dllimport.
		# Any target linking this becomes the engine's export side too, so link it only into the decoder.
		target_compile_definitions(snk_model_decoder_side PUBLIC SNK_ENGINE_BUILD)
	endif()
endif()
