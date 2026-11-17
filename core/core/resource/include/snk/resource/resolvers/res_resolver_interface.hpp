#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

namespace core::res
{
	class res_resolver_interface
	{
	public:
		using resource_changed_callback = std::function<void(const ::res::tag& tag)>;
		// A batch of changes below the mount, from the io thread (the watcher's poll, or an operation done through the
		// resolver). Only set for a mount that reports changes.
		using changes_callback = std::function<void(std::vector<::res::file_change>)>;
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

		// store(), but returns only once the data is where resolve() reads it from (on disk for a file resolver), and
		// whether it got there: a caller that must not lose the data (a saved level) waits for this.
		virtual bool store_sync(const ::res::tag& tag, const std::vector<std::byte>& data) {
			return store(tag, data);
		}


		// store_sync() that never replaces a file: refused when one is there, also when it appears meanwhile.
		virtual ::res::file_operation create_sync(const ::res::tag& tag, const std::vector<std::byte>& data) {
			(void)tag;
			(void)data;
			return ::res::file_operation::failure("this protocol cannot create files");
		}

		// Removes the data stored under the tag; false when this resolver cannot (the default) or the tag is not its own.
		virtual bool erase(const ::res::tag& tag) {
			(void)tag;
			return false;
		}

		// Removes the file of `tag` for good and returns once it is gone. Refused when this resolver cannot (the default),
		// is read-only, or its write policy does not cover the tag.
		virtual ::res::file_operation remove(const ::res::tag& tag) {
			(void)tag;
			return ::res::file_operation::failure("this protocol cannot remove files");
		}

		// Moves the file of `from` to `to` (both of this resolver's protocol), never over an existing file, and returns once
		// it is done. Refused like remove(), and when either side is outside the write policy.
		virtual ::res::file_operation rename(const ::res::tag& from, const ::res::tag& to) {
			(void)from;
			(void)to;
			return ::res::file_operation::failure("this protocol cannot rename files");
		}

		virtual bool exists(::res::tag const& tag) const {
			(void)tag;
			return false;
		}

		void set_changes_callback(changes_callback&& callback) {
			std::lock_guard lock(m_on_changes_mutex);
			m_on_changes = std::move(callback);
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

		void notify_changes(std::vector<::res::file_change> batch) {
			if (batch.empty()) {
				return;
			}
			changes_callback callback;
			{
				std::lock_guard lock(m_on_changes_mutex);
				callback = m_on_changes;
			}
			if (callback) {
				callback(std::move(batch));
			}
		}

		std::mutex m_on_changes_mutex;
		changes_callback m_on_changes;
		std::mutex m_on_resource_changed_mutex;
		resource_changed_callback m_on_resource_changed;
		std::string protocol;
	};

}