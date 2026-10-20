#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/driver/rnd_render_mode.h>
#include <snk/resource/res_tag.h>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/describe.hpp>
#include <snk/common/common.h>
#include <snk/common/snk_api.h>

namespace scn
{
	enum class pass_queue { NONE, OPAQUE, TRANSPARENT, MIX };

	BOOST_DESCRIBE_ENUM(pass_queue, NONE, OPAQUE, TRANSPARENT, MIX)

	class material_desc;
}

// The schema of the material: keys and hints of the fields, the defaults come from the members of the class. Declared before
// the class so that the serializer generated for schema_desc sees the attributes. The order of the members is the order of
// the keys in the file.
namespace desc
{
	// The sampler slots are positional (diffuse, specular, normal), and a hole in the middle is a null written by the model
	// importer ("samplers": [null, "res://spec.png"]). The array of the schema would refuse a null element and write an
	// empty tag as "", so the list is a custom field: an empty tag is a null in both directions, any element that is not a
	// string reads as an empty tag.
	template<>
	struct json_codec<std::vector<res::tag>> {
		static json::value to_json(const std::vector<res::tag>& tags)
		{
			json::array array;
			array.reserve(tags.size());
			for (const auto& tag : tags)
				array.push_back(tag.is_valid() ? json::value(json::string(tag.view())) : json::value());
			return json::value(std::move(array));
		}

		static bool from_json(std::vector<res::tag>& tags, const json::value& in)
		{
			if (!in.is_array())
				return false;
			std::vector<res::tag> parsed;
			parsed.reserve(in.as_array().size());
			for (const auto& item : in.as_array())
				parsed.push_back(item.is_string() ? res::tag{ std::string_view{ item.as_string() } } : res::tag{});
			tags = std::move(parsed);
			return true;
		}
	};

	template<>
	inline constexpr auto field_attrs<scn::material_desc> = attrs(
		f("defines").tooltip("Shader preprocessor defines"),
		f("samplers_textures").key("/samplers").kind(field_kind::custom).tooltip("Texture tags by sampler slot; an empty slot is null"),
		f("vertex_shader").key("/shader_vertex").write_null_if_empty(),
		f("fragment_shader").key("/shader_fragment").write_null_if_empty(),
		f("geometry_shader").key("/shader_geometry").write_null_if_empty(),
		f("queue").write_null_if_empty().tooltip("Draw queue: opaque, transparent, or mix for both; null draws in none"),
		f("albedo").key("/uniforms/albedo").color(),
		f("emissive").key("/uniforms/emissive").color(),
		// Written only when it is not the default. It used to be read but never written, so a changed value was lost on save (#321).
		f("shininess").key("/uniforms/shininess").range(0, 1000, 1).write_if_not_default(),
		f("render_mode"));

	// Lower-case words in the desc; an unknown word reads as no queue, like null.
	template<>
	struct enum_names<scn::pass_queue> {
		static constexpr std::string_view name = "pass_queue";
		static constexpr std::array<std::pair<scn::pass_queue, std::string_view>, 3> overrides{ {
			{ scn::pass_queue::OPAQUE, "opaque" },
			{ scn::pass_queue::TRANSPARENT, "transparent" },
			{ scn::pass_queue::MIX, "mix" },
		} };
		static constexpr scn::pass_queue fallback = scn::pass_queue::NONE;
		static constexpr scn::pass_queue null_value = scn::pass_queue::NONE;
	};

	// The names of the topologies of the driver; an unknown word reads as TRIANGLE (material_desc::deserialize warns
	// about the ones that were removed from the driver in RHI R4).
	template<>
	struct enum_names<rnd::driver::RENDER_MODE> {
		static constexpr std::string_view name = "render_mode";
		static constexpr rnd::driver::RENDER_MODE fallback = rnd::driver::RENDER_MODE::TRIANGLE;
	};
}

namespace scn
{
	class SNK_API material_desc : public desc::schema_desc<material_desc, desc::schema_kind::desc>
	{
	public:
		static constexpr std::string_view __type = "material_desc";
		using base_type = desc::desc_base;

		virtual ~material_desc() = default;
		material_desc() = default;
		material_desc(const material_desc&) = default;
		material_desc(material_desc&&) = default;
		material_desc& operator=(const material_desc&) = default;
		material_desc& operator=(material_desc&&) = default;

		// Member-wise, so that the members the schema does not list (ambient, specular) are copied too.
		virtual void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}

		// Generic read of the schema, after the one legacy form it cannot say: see scn_material_desc.cpp.
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;

		std::vector<std::string> defines;
		std::vector<res::tag> samplers_textures;
		res::tag vertex_shader;
		res::tag fragment_shader;
		res::tag geometry_shader;
		pass_queue queue = pass_queue::OPAQUE;
		ds::color albedo = ds::color(1.0f);
		ds::color emissive = ds::color(0.0f);
		float shininess = 32.0f; // 0.0 -> 1000
		rnd::driver::RENDER_MODE render_mode = rnd::driver::RENDER_MODE::TRIANGLE;

		// Not in the file: neither read nor written.
		ds::color specular = ds::color(1.0f);
		ds::color ambient = ds::color(1.0f);

		BOOST_DESCRIBE_CLASS(material_desc, (), (defines, samplers_textures, vertex_shader, fragment_shader, geometry_shader,
			queue, albedo, emissive, shininess, render_mode), (), ())
	};
}
