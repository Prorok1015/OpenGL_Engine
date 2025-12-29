#pragma once
#include "res_tag.h"
#include "res_control_block.hpp"
#include <functional>
#include <future>

namespace core::res
{
	class res_resolver_interface
	{
	public:
		using resource_changed_callback = std::function<void(const ::res::tag& tag)>;
		using async_raw_data = std::shared_ptr<res_control_block<std::vector<std::byte>>>;

		virtual ~res_resolver_interface() = default;
		virtual async_raw_data resolve(const ::res::tag& tag) const = 0;

		void set_resource_changed_callback(resource_changed_callback&& callback) {
			m_on_resource_changed = std::move(callback);
		}

	protected:
		resource_changed_callback m_on_resource_changed;
	};

}