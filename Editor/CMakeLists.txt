cmake_minimum_required(VERSION 3.16)
project(editor)
set(CMAKE_CXX_STANDARD 20)
set(SE_RESOURCE_ABSOLUTE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/")
set(WITH_CONSOLE_MODE TRUE)

if(${WITH_CONSOLE_MODE})  
	add_executable(${PROJECT_NAME} "main.cpp")
	target_compile_definitions(${PROJECT_NAME} PUBLIC ENGINE_CONSOLE_MODE=1)
else()
	add_executable(${PROJECT_NAME} WIN32 "main.cpp")
endif()

add_subdirectory("code/editor_system")
target_link_libraries(${PROJECT_NAME} PRIVATE editor_system PRIVATE engine PRIVATE config_system)

