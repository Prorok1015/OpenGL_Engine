cmake_minimum_required(VERSION 3.16)
project(core_input)

add_library(${PROJECT_NAME} INTERFACE)
add_library(core::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
#add_library(${PROJECT_NAME} STATIC)

#file(GLOB_RECURSE SE_WINDOW_EI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
#target_sources(${PROJECT_NAME} PRIVATE ${SE_WINDOW_EI_SOURCES})

#target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} INTERFACE common)

