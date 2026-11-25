#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/model_codec/res_tag.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <boost/describe.hpp>
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>

// A shader program as a resource (.shader.desc, #1199): its stages and preprocessor defines. Parameters and samplers come
// later; a material refers to one through material_desc::shader (#1200).
namespace rnd
{
	// The stages of one program; an empty tag: the stage is not named.
	struct shader_stage_sources
	{
		res::tag vertex;
		res::tag fragment;
		res::tag geometry;
	};
	BOOST_DESCRIBE_STRUCT(shader_stage_sources, (), (vertex, fragment, geometry))

	class shader_desc;

	// The graphics backends a .shader.desc may name in "backends"; another name is kept with a load warning.
	inline constexpr std::string_view k_shader_backends[] = { "opengl", "vulkan", "d3d12", "metal" };
}

// Declared before the class so that the serializer generated for schema_desc sees the attributes.
namespace desc
{
	template<>
	inline constexpr auto field_attrs<rnd::shader_stage_sources> = attrs(
		f("vertex").write_if_valid().tooltip("Vertex stage of this backend; empty: the shared one"),
		f("fragment").write_if_valid().tooltip("Fragment stage of this backend; empty: the shared one"),
		f("geometry").write_if_valid().tooltip("Geometry stage of this backend; empty: the shared one"));

	template<>
	inline constexpr auto field_attrs<rnd::shader_desc> = attrs(
		f("vertex").write_null_if_empty().tooltip("Vertex stage"),
		f("fragment").write_null_if_empty().tooltip("Fragment stage"),
		f("geometry").write_if_valid().tooltip("Geometry stage; empty: none"),
		f("defines").tooltip("Preprocessor defines, one per entry: NAME or NAME VALUE"),
		// Not if_valid: an empty map is the JSON {}, which if_valid writes; if_not_default leaves it out.
		f("backends").ref("shader_stage_sources").write_if_not_default()
			.tooltip("Stage sources per graphics backend (opengl, vulkan, d3d12, metal) over the shared ones; the runtime does not select a backend yet (#1029)"));
}

namespace rnd
{
	class SNK_API shader_desc : public desc::schema_desc<shader_desc, desc::schema_kind::desc>
	{
	public:
		static constexpr std::string_view __type = "shader_desc";
		using base_type = desc::desc_base;

		// The program and defines the material compiler builds from the same stages (without its skinning define).
		// `extra_defines` are the material's: the defines are the union of both, sorted, without duplicates. Sorting changes
		// the order of the `#define` lines against the files, and that is intended: the same set always gives the same
		// constant_data, so operator== (order-sensitive) agrees with variant_hash (order-independent). Two entries for the
		// same NAME with different text: the extra (material) one is kept, with a warning.
		shader_config config(const std::vector<std::string>& extra_defines = {}) const;

		// The schema read, then the stages are checked (a missing vertex or fragment, an extension that is not the stage's):
		// load warnings, the values are kept as written.
		virtual void deserialize(desc::desc_system& desc_system, const json::object& data) override;

		// The shared stages with the stages `backend` overrides on top; an unknown or absent backend: the shared stages.
		// Not used by the runtime yet (#1029).
		shader_stage_sources stages_for(std::string_view backend) const;

		res::tag vertex;
		res::tag fragment;
		res::tag geometry;
		// Each entry is written as `#define <entry>`: "NAME" or "NAME VALUE" (not "NAME=VALUE", which defines NAME as "=VALUE").
		std::vector<std::string> defines;
		// Backend name -> the stages it replaces (#1203). Last: appended after the fields of #1199.
		std::map<std::string, shader_stage_sources> backends;

		BOOST_DESCRIBE_CLASS(shader_desc, (), (vertex, fragment, geometry, defines, backends), (), ())
	};
}
