#pragma once
#include "res_tag.h"

namespace res
{
	class ResourceSystem;
	class Resource
	{
		friend res::ResourceSystem;
	public:
		Resource(const Tag& tag) : tag_(tag) {}

		bool operator== (const Tag& tag) const { return tag_ == tag; }

		res::Tag get_tag() const { return tag_; }

	protected:
		Tag tag_;
	};

	using RecourceRef = std::shared_ptr<Resource>;
}
