#pragma once
#include "res_tag.h"

namespace res
{
	class resource_system;
	class resource_entry
	{
		friend res::resource_system;
	public:
		resource_entry(const tag& tag) : tag_(tag) {}

		bool operator== (const tag& tag) const { return tag_ == tag; }

		res::tag get_tag() const { return tag_; }

	protected:
		tag tag_;
	};

	using RecourceRef = std::shared_ptr<resource_entry>;
}
