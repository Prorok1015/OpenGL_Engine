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

		res_resolver_interface(const std::string_view prt)
			: protocol(prt) {
		}

		virtual ~res_resolver_interface() = default;
		virtual async_raw_data resolve(const ::res::tag& tag) const = 0;
		virtual bool store(const ::res::tag& tag, const std::vector<std::byte>& data) {
			(void)tag;
			(void)data;
			return false;
		}

		virtual bool exists(::res::tag const& tag) const {
			(void)tag;
			return false;
		}

		void set_resource_changed_callback(resource_changed_callback&& callback) {
			m_on_resource_changed = std::move(callback);
		}

		void set_protocol(const std::string& prt) {
			protocol = prt;
		}

	protected:
		resource_changed_callback m_on_resource_changed;
		std::string protocol;
	};

}