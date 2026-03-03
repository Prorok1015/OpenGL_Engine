#pragma once
#include "rnd_texture_desc.h"
#include "resources/res_resource_picture.h"

namespace rnd
{
	class texture_2d_desc : public texture_desc
	{
	public:
		using base_type = texture_desc;
		texture_2d_desc() = default;
		virtual ~texture_2d_desc() override = default;
		texture_2d_desc(const texture_2d_desc&) = default;
		texture_2d_desc(texture_2d_desc&&) noexcept = default;
		texture_2d_desc& operator=(const texture_2d_desc&) = default;
		texture_2d_desc& operator=(texture_2d_desc&&) noexcept = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			texture_2d_desc& other_desc = static_cast<texture_2d_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(json::object& resource) const override;

		res::res_handle<res::picture_resource> get_picture_handle() const { return picture_handle; }

	private:
		res::res_handle<res::picture_resource> picture_handle;
	};
}