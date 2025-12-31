#pragma once
#include "common.h"
#include "res_tag.h"
#include "logger/engine_log.h"
#include "resources/res_resource_base.h"
#include "resolvers/res_memory_resolver.h"
#include "adapters/res_adapter_info.hpp"
#include "resolvers/res_resolver_interface.hpp"
#include "resolvers/res_resource_handle.hpp"
#include "adapters/res_adapter_worker_base.hpp"
#include <future>
#include <iostream>

namespace res
{
	template<class T>
	using res_handle = core::res::res_handle<T>;

	class resource_system
	{
	public:
		static std::string get_absolut_path(const tag& tag);
		template<class T>
		using resource_handle_t = core::res::res_control_block<std::shared_ptr<T>>;
		using resource_handle = core::res::res_control_block_base;
		using cache_entry = std::weak_ptr<resource_handle>;

		using protocol = std::string_view;
		using extension = adapter_info;

		using data = core::res::res_resolver_interface::async_raw_data;
		using resolver = std::shared_ptr<core::res::res_resolver_interface>;

		using adapter = std::function<std::shared_ptr<resource_entry>(const res::tag&, const std::vector<std::byte>&)>;

		using reload_callback = std::function<void(const res::tag&)>;

	public:
		resource_system();
		~resource_system() = default;
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		static std::filesystem::path get_resources_path();

		void set_adapter_worker(std::unique_ptr<core::res::adapter_worker_base>&& worker) {
			adapter_worker = std::move(worker);
			adapter_worker->start();
		}

		void watch(const res::tag& tag, void* owner, reload_callback cb) {
			std::lock_guard lock(cache_mutex);
			watchers[tag][owner] = std::move(cb);
		}

		void unwatch(const res::tag& tag, void* owner) {
			std::lock_guard lock(cache_mutex);
			if (watchers.contains(tag)) {
				watchers[tag].erase(owner);
			}
		}

		void signal_changed(const res::tag& tag) {
			cache.erase(tag);

			if (watchers.contains(tag)) {
				auto callbacks = watchers.at(tag);

				for (auto& [owner, callback] : callbacks) {
					callback(tag);
				}
			}
		}

		template<class T, class... ARGS>
		T& registrate_resolver(protocol prt, ARGS... args) {
			ASSERT_MSG(resolvers.find(prt) == resolvers.end(), "That protocol already has resolver!");
			return *std::static_pointer_cast<T>(resolvers[prt] = std::make_unique<T>(std::forward<ARGS>(args)...));
		}

		void unregistrate_resolver(protocol prt) {
			resolvers.erase(prt);
		}

		void registrate_adapter(extension extension, adapter loader) {
			ASSERT_MSG(adapters.find(extension) == adapters.end(), "That extension already has adapter!");
			adapters[extension] = std::move(loader);
		}

		void unregistrate_adapter(extension ext) {
			adapters.erase(ext);
		}

		data require_resource_data(const tag& tag) const
		{
			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return data{};
			}

			return resolvers.at(tag.protocol())->resolve(tag);
		}

		template<class RESOURCE>
		auto require_resource(const res::tag& tag)
		{
			if (auto cached_res = try_get_cached_resource(tag)) {
				return res_handle<RESOURCE>(
					std::static_pointer_cast<resource_handle_t<RESOURCE>>(cached_res)
				);
			}

			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return res_handle<RESOURCE>{};
			}

			auto res_info = res::resource_info::make<RESOURCE>(tag.extension());
			auto adapter = std::find_if(adapters.begin(), adapters.end(), [&res_info](const auto& par) { return par.first == res_info; });
			if (adapter == adapters.end()) {
				egLOG("resource/require", "Extention '{}' is not supported!", tag.extension());
				return res_handle<RESOURCE>{};
			}

			auto final_cb = std::make_shared<resource_handle_t<RESOURCE>>();
			push_resource_to_cache(tag, std::static_pointer_cast<resource_handle>(final_cb));

			start_async_loading<RESOURCE>(tag, final_cb);

			return res_handle<RESOURCE>(final_cb);
		}

	private:
		std::shared_ptr<resource_handle> try_get_cached_resource(const tag& tag) {
			std::lock_guard lock(cache_mutex);
			auto it = cache.find(tag);
			if (it != cache.end()) {
				if (auto sp = it->second.lock()) {
					return sp;
				}
				cache.erase(it);
			}
			return nullptr;
		}

		void push_resource_to_cache(const tag& tag, const std::shared_ptr<resource_handle>& resource) {
			std::lock_guard lock(cache_mutex);
			cache[tag] = resource;
		}

		template<typename T>
		void start_async_loading(res::tag tag, std::shared_ptr<core::res::res_control_block<std::shared_ptr<T>>> final_cb) {
			auto& resolver = resolvers[tag.protocol()];

			auto raw_data_block = resolver->resolve(tag);
			ASSERT_MSG(raw_data_block, "resource data not found");

			auto adapter_cb = [this, tag, final_cb, raw_data_block]() {
				if (raw_data_block->status == core::res::res_status::error) {
					final_cb->set_error("Resolver error: "s + raw_data_block->error_msg);
					return;
				}

				try {
					auto res_info = res::resource_info::make<T>(tag.extension());
					auto adapter_it = std::find_if(adapters.begin(), adapters.end(), [&res_info](const auto& par) { return par.first == res_info; });
					auto& adapter = adapter_it->second;

					final_cb->status = core::res::res_status::processing;
					auto parsed_obj = std::static_pointer_cast<T>(adapter(tag, raw_data_block->data));

					final_cb->set_ready(std::move(parsed_obj));
				}
				catch (const std::exception& e) {
					final_cb->set_error("Adapter error: "s + e.what());
				}
			};

			raw_data_block->then([this, adapter_cb](auto& raw_block) {
				post_adapter_work(adapter_cb);
			});
		}

		void post_adapter_work(std::function<void()> cb);

	private:
		std::unordered_map<protocol, resolver> resolvers;
		std::unordered_map<extension, adapter> adapters;
		std::unordered_map<tag, cache_entry> cache;
		std::unordered_map<res::tag, std::unordered_map<void*, reload_callback>> watchers;
		mutable std::mutex cache_mutex;
		std::unique_ptr<core::res::adapter_worker_base> adapter_worker = nullptr;
	public:
		memory_resolver& memory_resolver_;
	};

	resource_system& get_system();
}
