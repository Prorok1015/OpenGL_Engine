cmake_minimum_required(VERSION 3.16)
project(snk_model_decoder)

# Reads one model file into descs in a process of its own (see main.cpp, #647). Console tool: no window, no GPU.
# Without assimp there is nothing to decode, so the tool is not built.
if(NOT SNK_WITH_ASSIMP)
	return()
endif()

# snk_model_decoder is what ships. snk_model_decoder_selftest is the same program built with the --self-test-* flags
# (sleep, allocate, start a helper copy) for the process tests of the runner; it is never installed.
foreach(_target snk_model_decoder snk_model_decoder_selftest)
	add_executable(${_target} main.cpp)
	target_link_libraries(${_target} PRIVATE snk::model_import snk::engine snk::engine_api snk::config_system snk::sanitizers)
	if(SNK_ENGINE_SHARED AND MSVC)
		target_link_libraries(${_target} PRIVATE Boost::json)
	endif()
	if(WIN32 AND SNK_ENGINE_SHARED)
		# Windows has no rpath: snk_engine.dll must sit next to the executable.
		add_custom_command(TARGET ${_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${_target}> $<TARGET_FILE_DIR:${_target}>
			COMMAND_EXPAND_LISTS)
	endif()
endforeach()
target_compile_definitions(snk_model_decoder_selftest PRIVATE SNK_MODEL_DECODER_SELF_TEST=1)

# Next to editor.exe in the editor package (component `editor`): the editor starts it from its own folder. A static
# engine has no editor to ship it with.
if(SNK_ENGINE_SHARED)
	install(TARGETS snk_model_decoder RUNTIME DESTINATION bin COMPONENT editor)
	if(MSVC)
		install(FILES $<TARGET_PDB_FILE:snk_model_decoder> DESTINATION bin OPTIONAL COMPONENT editor)
	endif()
endif()
