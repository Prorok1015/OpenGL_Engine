cmake_minimum_required(VERSION 3.16)
project(drv_validation)
add_library(drv_validation STATIC)
add_library(snk::drv_validation ALIAS drv_validation)

file(GLOB_RECURSE SE_DRV_VALIDATION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_VALIDATION_SOURCES})
# Public headers: include/snk/driver_validation/ (rnd_validation_driver.h is what render's backend selection
# wraps the driver with; it includes rnd_validation_objects.h and rnd_validation_validator.h, so they are public too).
snk_component_headers(${PROJECT_NAME} SDK_INTERNAL)

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::drv_interface PRIVATE snk::engine_logger)
