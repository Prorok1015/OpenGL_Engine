#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/resource/res_tag.h>
#include <string>
#include <vector>
#include <snk/common/snk_api.h>

namespace scn
{
	enum class pass_queue { NONE, OPAQUE, TRANSPARENT, MIX };

	// Mirrors rnd::driver::RENDER_MODE enumerators exactly (same order/names). Kept scene-side so
	// material_desc stays a POD with no render/driver header dependency (EPIC-49 US-49-5). The
	// render-side material compiler maps this to rnd::driver::RENDER_MODE when needed.
	enum class material_render_mode
	{
		TRIANGLE,
		TRIANGLE_STRIP,
		TRIANGLE_FAN,
		TRIANGLE_ADJ,
		TRIANGLE_STRIP_ADJ,
		LINE,
		LINE_STRIP,
		LINE_LOOP,
		LINE_ADJ,
		LINE_STRIP_ADJ,
		POINT,
	};

	class SNK_API material_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "material_desc";
		using base_type = desc::desc_base;

		virtual ~material_desc() = default;
		material_desc() = default;

		virtual void copy_to(desc::desc_base& other) const override
		{
			material_desc& other_desc = static_cast<material_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		std::vector<res::tag> samplers_textures;
		ds::color albedo = ds::color(1.0f);
		ds::color specular = ds::color(1.0f);
		ds::color ambient = ds::color(1.0f);
		ds::color emissive = ds::color(0.0f);
		float shininess = 32.0f; // 0.0 -> 1000

		pass_queue queue = pass_queue::OPAQUE;
		material_render_mode render_mode = material_render_mode::TRIANGLE;

		res::tag vertex_shader;
		res::tag fragment_shader;
		res::tag geometry_shader;
		std::vector<std::string> defines;
	};

	void tag_invoke(json::value_from_tag, json::value& out, const scn::pass_queue& c);
	scn::pass_queue tag_invoke(json::value_to_tag<scn::pass_queue>, const json::value& obj);
}
