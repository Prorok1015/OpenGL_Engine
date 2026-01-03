#pragma once
#include "common.h"
#include "res_resource_base.h"

namespace res
{
	class picture_resource : public resource_entry
	{
	public:
		picture_resource(const tag& tag, glm::ivec2 size_, int channels_, unsigned char* data_);
		virtual ~picture_resource() override;
		picture_resource(const picture_resource&);
		picture_resource& operator= (const picture_resource&);
		picture_resource(picture_resource&&) noexcept;
		picture_resource& operator= (picture_resource&&) noexcept;

		unsigned char* data() const { return data_; }
		int channels() const { return channels_; }
		const glm::ivec2& size() const { return size_; }

	private:
		unsigned char* data_ = nullptr;
		int channels_ = 0;
		glm::ivec2 size_{ 0, 0 };
		bool is_embedded_picture = false;
	};

	using ImageRef = std::shared_ptr<picture_resource>;
}
