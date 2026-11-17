cmake_minimum_required(VERSION 3.16)
project(model_codec)

# snk_model_codec (#1077): the pure encoders an import needs (res::tag and its JSON, the glm JSON, the desc binary file and
# the geometry desc with its binary file) and nothing that reads, stores, opens a window or draws. snk_engine carries it
# (core/engine/CMakeLists.txt) and resource, render and scene link it; the model decoder links it alone, without the
# engine (tools/model_decoder). Its symbols keep their names and SNK_API: they only moved out of resource, render and scene.
add_library(snk_model_codec STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/res_asset_id.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/res_desc_binary_codec.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/res_reference.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/res_tag.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/rnd_geometry_encode.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/scn_glm_json_convert.cpp")
add_library(snk::model_codec ALIAS snk_model_codec)
set_property(TARGET snk_model_codec PROPERTY EXPORT_NAME model_codec)

snk_component_headers(snk_model_codec)

# drv_interface: BufferLayout (header-only) of the geometry.
target_link_libraries(snk_model_codec PUBLIC snk::common PUBLIC snk::drv_interface PUBLIC Boost::json PRIVATE snk::engine_logger)
