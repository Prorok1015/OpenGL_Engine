#pragma once
#include <chrono>

namespace gui {
	struct layer_interface {
		virtual ~layer_interface() = default;

		virtual void on_attach() {};
		virtual void on_dettach() {};
		virtual void on_update(std::chrono::duration<float> dt) = 0;
	};
}