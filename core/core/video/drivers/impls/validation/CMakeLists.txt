cmake_minimum_required(VERSION 3.16)
project(drv_validation)
add_library(drv_validation STATIC)
add_library(snk::drv_validation ALIAS drv_validation)

file(GLOB_RECURSE SE_DRV_VALIDATION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_VALIDATION_SOURCES})
# Every header of the decorator is internal (rnd_validation_*): the source dir is private to the target.
# render and unit_tests get explicit access to it through the SOURCE_DIR property.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::drv_interface PRIVATE snk::engine_logger)
