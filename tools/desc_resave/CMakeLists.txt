cmake_minimum_required(VERSION 3.16)
project(desc_resave)

# Loads every .desc under a resources directory through the engine and writes it back in the current
# format (see main.cpp). Console tool: no window, no GPU.
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::config_system PRIVATE snk::sanitizers)
