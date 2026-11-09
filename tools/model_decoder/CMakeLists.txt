cmake_minimum_required(VERSION 3.16)
project(snk_model_decoder)

# Reads one model file into descs in a process of its own (see main.cpp, #647). Console tool: no window, no GPU.
# Without assimp there is nothing to decode, so the tool is not built.
if(NOT SNK_WITH_ASSIMP)
	return()
endif()
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::model_import snk::engine snk::engine_api snk::config_system snk::sanitizers)
if(SNK_ENGINE_SHARED AND MSVC)
	target_link_libraries(${PROJECT_NAME} PRIVATE Boost::json)
endif()
if(WIN32 AND SNK_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
		COMMAND_EXPAND_LISTS)
endif()

# Next to editor.exe in the editor package (component `editor`): the editor starts it from its own folder. A static
# engine has no editor to ship it with.
if(SNK_ENGINE_SHARED)
	install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin COMPONENT editor)
	if(MSVC)
		install(FILES $<TARGET_PDB_FILE:${PROJECT_NAME}> DESTINATION bin OPTIONAL COMPONENT editor)
	endif()
endif()
