#pragma once
#include "common.h"

struct GLFWwindow;

namespace wnd
{
	class context final
	{
	public:
		struct header
		{
			std::string title = "Default title";
			glm::ivec2 size = glm::ivec2{ 1920, 1080 }; 
		};

	public:
		context() = default;
		context(header title);
		~context();

		context(wnd::context&& ctx_) noexcept {
			operator=(std::move(ctx_));
		}

		context(const wnd::context&) = delete;
		context& operator= (const wnd::context&) = delete;
		context& operator= (wnd::context&& ctx_) noexcept
		{
			std::swap(title, ctx_.title);
			std::swap(frame_count, ctx_.frame_count);
			std::swap(internal_id, ctx_.internal_id);

			return *this;
		}
		 
		void swap_buffers();

		const header& get_header() const { return title; }
		GLFWwindow* get_id() const { return internal_id; }

	protected:
		void set_next_frame() { ++frame_count; } 

	private:
		header title;
		std::uint64_t frame_count = 0;
		GLFWwindow* internal_id = nullptr; 
	};
}
