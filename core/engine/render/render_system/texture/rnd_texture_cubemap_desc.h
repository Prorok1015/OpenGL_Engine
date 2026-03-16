#pragma once
#include "rnd_texture_desc.h"
#include "resources/res_resource_picture.h"

namespace rnd
{
	class texture_cubemap_desc : public texture_desc
	{
	public:
		using base_type = texture_desc;

		struct faces {
			res::res_handle<res::picture_resource> right;
			res::res_handle<res::picture_resource> left;
			res::res_handle<res::picture_resource> top;
			res::res_handle<res::picture_resource> bottom;
			res::res_handle<res::picture_resource> front;
			res::res_handle<res::picture_resource> back;
		};

		enum class SOURCE_TYPE {
			SIX_FACES,
			SINGLE_FILE
		};

		virtual ~texture_cubemap_desc() = default;
		texture_cubemap_desc() = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			texture_cubemap_desc& other_desc = static_cast<texture_cubemap_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		const faces& get_faces() const { return cube_faces; }
		const res::res_handle<res::picture_resource>& get_single_image() const { return single_image; }
		SOURCE_TYPE get_source_type() const { return mode; }

	private:
		SOURCE_TYPE mode = SOURCE_TYPE::SIX_FACES;
		res::res_handle<res::picture_resource> single_image;
		faces cube_faces;
	};
}