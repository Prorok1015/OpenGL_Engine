cmake_minimum_required(VERSION 3.16)
project(drv_null)
add_library(drv_null STATIC)
add_library(snk::drv_null ALIAS drv_null)

file(GLOB_RECURSE SE_DRV_NULL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_NULL_SOURCES})
# Every header of the driver is internal (rnd_null_*): the source dir is private to the target.
# render and unit_tests get explicit access to it through the SOURCE_DIR property.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::drv_interface PRIVATE snk::engine_logger)
