cmake_minimum_required(VERSION 3.16)
project(drv_null)
add_library(drv_null STATIC)
add_library(snk::drv_null ALIAS drv_null)

file(GLOB_RECURSE SE_DRV_NULL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_NULL_SOURCES})
# Public header: include/snk/driver_null/rnd_null_swapchain.h (what render's backend selection creates).
# The driver and its objects (rnd_null_driver.h, rnd_null_objects.h) are internal: unit_tests reach them
# through the SOURCE_DIR property.
snk_component_headers(${PROJECT_NAME} SDK_INTERNAL)

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::drv_interface PRIVATE snk::engine_logger)
