cmake_minimum_required(VERSION 3.16)
project(snk_model_decoder)

# Reads one model file into descs in a process of its own (see main.cpp, #647). Console tool: no window, no GPU.
# Without assimp there is nothing to decode, so the tool is not built.
if(NOT SNK_WITH_ASSIMP)
	return()
endif()

# snk_model_decoder is what ships. snk_model_decoder_selftest is the same program built with the --self-test-* flags
# (sleep, allocate, start a helper copy) for the process tests of the runner; it is never installed.
# Neither links snk_engine nor loads a DLL of the project (#1077, groundwork for win32k lockdown #1054): the decoder side of
# the editor's import (snk_model_decoder_side, Editor/code/model_import) with the import core, snk_model_codec, common, the
# logger, platform and assimp, all static. In a shared build every part of it is compiled as the engine's export side
# (SNK_ENGINE_BUILD, PUBLIC from snk_model_decoder_side): SNK_API is never dllimport here. tools/ci/check_decoder_deps.sh
# and check_windows_deps.sh --deny check the binary.
foreach(_target snk_model_decoder snk_model_decoder_selftest)
	add_executable(${_target} main.cpp decoder_sandbox.cpp)
	target_link_libraries(${_target} PRIVATE snk::model_decoder_side snk::platform snk::engine_logger snk::sanitizers)
	if(WIN32)
		# The DLLs it loads from its own folder, by name (#1054): the editor copies exactly these with it when its
		# AppContainer cannot read the folder it is in (a dev build). The C/C++ runtime beside it is copied too.
		file(GENERATE OUTPUT "$<TARGET_FILE_DIR:${_target}>/${_target}.deps"
			CONTENT "$<JOIN:$<PATH:GET_FILENAME,$<TARGET_RUNTIME_DLLS:${_target}>>,\n>\n")
	endif()
endforeach()
if(WIN32)
	# The --self-test-connect probe (#1054).
	target_link_libraries(snk_model_decoder_selftest PRIVATE ws2_32)
endif()
target_compile_definitions(snk_model_decoder_selftest PRIVATE SNK_MODEL_DECODER_SELF_TEST=1)

# Next to editor.exe in the editor package (component `editor`): the editor starts it from its own folder. A static
# engine has no editor to ship it with.
if(SNK_ENGINE_SHARED)
	install(TARGETS snk_model_decoder RUNTIME DESTINATION bin COMPONENT editor)
	if(UNIX AND NOT APPLE)
		set_target_properties(snk_model_decoder PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
	endif()
	if(MSVC)
		install(FILES $<TARGET_PDB_FILE:snk_model_decoder> DESTINATION bin OPTIONAL COMPONENT editor)
	endif()
	if(WIN32)
		install(FILES "$<TARGET_FILE_DIR:snk_model_decoder>/snk_model_decoder.deps" DESTINATION bin COMPONENT editor)
	endif()
endif()
