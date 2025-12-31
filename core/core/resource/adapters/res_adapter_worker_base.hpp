#pragma once
#include <functional>

namespace core::res
{
	class adapter_worker_base
	{
	public:
		virtual ~adapter_worker_base() = default;
		virtual void start() = 0;
		virtual void stop() = 0;
		virtual bool is_stopped() const = 0;
		virtual void post(std::function<void()> cb) = 0;
	};
}