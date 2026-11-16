cmake_minimum_required(VERSION 3.16)
project(model_import)

# snk_model_import: reading a model file into the engine's descs (through snk::model_import_assimp) and what guards
# what a model may make the editor read (model_file_access, file_identity). It knows nothing of the editor: editor_system links it for the
# import of a person and of an agent, and the decoder process (#647) will link it too, so the same code runs on both
# sides of the process boundary.
add_library(${PROJECT_NAME} STATIC)
add_library(snk::model_import ALIAS ${PROJECT_NAME})

set(SE_MODEL_IMPORT_SOURCES edt_decoder_sandbox.cpp edt_decode_bundle.cpp edt_decode_codec.cpp edt_decode_runner.cpp edt_process_decoder.cpp edt_model_file_access.cpp edt_person_model_import.cpp)
if(SNK_WITH_ASSIMP)
	list(APPEND SE_MODEL_IMPORT_SOURCES edt_model_file_source.cpp edt_model_importer.cpp edt_decoded_model_adapter.cpp)
endif()
list(TRANSFORM SE_MODEL_IMPORT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(${PROJECT_NAME} PRIVATE ${SE_MODEL_IMPORT_SOURCES})
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
