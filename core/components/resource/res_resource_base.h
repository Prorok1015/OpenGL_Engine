#pragma once
#include "res_tag.h"

namespace res
{
	class resource_system;
	class Resource
	{
		friend res::resource_system;
	public:
		Resource(const tag& tag) : tag_(tag) {}

		bool operator== (const tag& tag) const { return tag_ == tag; }

		res::tag get_tag() const { return tag_; }

	protected:
		tag tag_;
	};

	using RecourceRef = std::shared_ptr<Resource>;
}
