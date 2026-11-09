# A game module compiled into a static host (#592): snk_add_game_host links it into test_static_game_host, which CI
# runs headless on project/ (build.yml, windows-package.yml). Static engine only, like the host itself.
add_library(static_game_module OBJECT static_game_module.cpp)
target_link_libraries(static_game_module PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::sanitizers)
snk_add_game_host(test_static_game_host MODULE static_game_module NAME static_game)
