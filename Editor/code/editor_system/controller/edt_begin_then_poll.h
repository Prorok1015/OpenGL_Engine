#pragma once
#include <functional>
#include <memory>
#include <utility>

namespace edt {

	// An implicit UI callback that calls `begin` once, on its first call, then `poll` on every call; `poll`'s result
	// keeps it registered. The state lives outside the functor: menu_layout_manager calls a copy of it each frame.
	inline std::function<bool()> begin_then_poll(std::function<void()> begin, std::function<bool()> poll)
	{
		auto pending = std::make_shared<std::function<void()>>(std::move(begin));
		return [pending, poll = std::move(poll)] {
			if (*pending) {
				std::exchange(*pending, nullptr)();
			}
			return poll();
		};
	}

}
