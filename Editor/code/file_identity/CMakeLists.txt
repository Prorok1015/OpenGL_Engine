cmake_minimum_required(VERSION 3.16)
project(file_identity)

# snk_file_identity: which file on the disk a path reaches, and SHA-256 of bytes. Neither engine nor importer: the asset
# database stamps its files with it (#888) and the model import guards what it reads, with assimp and without.
add_library(snk_file_identity STATIC "${CMAKE_CURRENT_SOURCE_DIR}/edt_file_identity.cpp")
add_library(snk::file_identity ALIAS snk_file_identity)
target_include_directories(snk_file_identity PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

target_link_libraries(editor_system PUBLIC snk_file_identity)
