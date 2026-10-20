#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <functional>
#include <future>
#include <mutex>

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


		// Removes the data stored under the tag; false when this resolver cannot (the default) or the tag is not its own.
		virtual bool erase(const ::res::tag& tag) {
			(void)tag;
			return false;
		}

		virtual bool exists(::res::tag const& tag) const {
			(void)tag;
			return false;
		}

		void set_resource_changed_callback(resource_changed_callback&& callback) {
			std::lock_guard lock(m_on_resource_changed_mutex);
			m_on_resource_changed = std::move(callback);
		}

		void set_protocol(const std::string& prt) {
			protocol = prt;
		}

	protected:
		// The callback may be set after a derived resolver already started its watcher thread,
		// so it is invoked from a copy taken under the lock, never in place.
		void notify_resource_changed(const ::res::tag& tag) {
			resource_changed_callback callback;
			{
				std::lock_guard lock(m_on_resource_changed_mutex);
				callback = m_on_resource_changed;
			}
			if (callback) {
				callback(tag);
			}
		}

		std::mutex m_on_resource_changed_mutex;
		resource_changed_callback m_on_resource_changed;
		std::string protocol;
	};

}