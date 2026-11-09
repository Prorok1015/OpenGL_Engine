# snk::game_host (#592): everything the shipping host's main() did, as a static library. A game executable links it
# with its compiled-in game module (snk_add_game_host, cmake/SnkGameHost.cmake); `game` links it with none.
add_library(game_host STATIC game_host.cpp game_host_module.cpp game_host_module.h include/snk/game_host/game_host.h)
add_library(snk::game_host ALIAS game_host)
snk_component_headers(game_host)
target_link_libraries(game_host PUBLIC snk::engine PUBLIC snk::engine_api PRIVATE snk::sanitizers)
