#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/model_codec/res_tag.h>
#include <string>
#include <string_view>
#include <vector>
#include <boost/describe.hpp>
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>

// A shader program as a resource (.shader.desc, #1199): its stages and preprocessor defines. Parameters and samplers come
// later; no material refers to it yet (#1200).
namespace rnd
{
	class shader_desc;
}

// Declared before the class so that the serializer generated for schema_desc sees the attributes.
namespace desc
{
	template<>
	inline constexpr auto field_attrs<rnd::shader_desc> = attrs(
		f("vertex").write_null_if_empty().tooltip("Vertex stage"),
		f("fragment").write_null_if_empty().tooltip("Fragment stage"),
		f("geometry").write_null_if_empty().tooltip("Geometry stage; null: none"),
		f("defines").tooltip("Shader preprocessor defines"));
}

namespace rnd
{
	class SNK_API shader_desc : public desc::schema_desc<shader_desc, desc::schema_kind::desc>
	{
	public:
		static constexpr std::string_view __type = "shader_desc";
		using base_type = desc::desc_base;

		// The program and defines the material compiler builds from the same stages (without its skinning define).
		shader_config config() const;

		res::tag vertex;
		res::tag fragment;
		res::tag geometry;
		std::vector<std::string> defines;

		BOOST_DESCRIBE_CLASS(shader_desc, (), (vertex, fragment, geometry, defines), (), ())
	};
}
