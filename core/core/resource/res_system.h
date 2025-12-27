#pragma once
#include "common.h"
#include "res_tag.h"
#include "logger/engine_log.h"
#include "resources/res_resource_base.h"
#include "resolvers/res_memory_resolver.h"
#include "adapters/res_adapter_info.hpp"
#include <future>
#include <iostream>

namespace res
{
	struct resolver_entry
	{
		using data = std::vector<std::byte>;
		std::function<std::future<data>(const tag&)> resolver;
		std::function<std::optional<tag>(const std::filesystem::path&)> path_mapper;
	};

	class resource_system 
	{
	public:
		static std::string get_absolut_path(const tag& tag);
		using resource_handle = std::shared_ptr<resource_entry>;
		using cache_entry = std::weak_ptr<resource_entry>;
		using protocol = std::string_view;
		using extension = adapter_info;
		using data = resolver_entry::data;
		using resolver = resolver_entry;
		using adapter = std::function<resource_handle(res::tag, const data&)>;
		using reload_callback = std::function<void(const res::tag&)>;

	public:
		resource_system();
		~resource_system() = default;
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		static std::filesystem::path get_resources_path();

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

		void signal_changed(const std::filesystem::path& path) {
			std::optional<res::tag> tag;
			{
				std::lock_guard lock(cache_mutex);
				for (const auto& [_, resolver] : resolvers) {
					if (tag = resolver.path_mapper(path)) {
						break;
					}
				}
				if (!tag.has_value()) {
					egLOG("resource/signal_changed", "Cannot find tag for changed path '{}'", path.string());
					return;
				}
			}

			cache.erase(tag.value());

			if (watchers.contains(tag.value())) {
				auto callbacks = watchers.at(tag.value());

				for (auto& [owner, callback] : callbacks) {
					callback(tag.value());
				}
			}
		}

		void registrate_resolver(protocol protocol, resolver resolver) {
			ASSERT_MSG(resolvers.find(protocol) == resolvers.end(), "That protocol already has resolver!");
			resolvers[protocol] = std::move(resolver);
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

		std::future<std::vector<std::byte>> require_resource_data(const tag& tag) const
		{
			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return std::future<std::vector<std::byte>>{};
			}

			return resolvers.at(tag.protocol()).resolver(tag);
		}

		template<class RESOURCE>
		auto require_resource(const res::tag& tag)
		{
			if (auto cached_res = try_get_cached_resource(tag)) {
				auto res_ptr = std::static_pointer_cast<RESOURCE>(cached_res);
				if (res_ptr) {
					return res_ptr;
				}
				egLOG("resource/require", "Cached resource '{}' has invalid type!", tag.string());
			}

			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return std::shared_ptr<RESOURCE>{};
			}

			auto res_info = res::resource_info::make<RESOURCE>(tag.extension());
			auto adapter = std::find_if(adapters.begin(), adapters.end(), [&res_info](const auto& par) { return par.first == res_info; });
			if (adapter == adapters.end()) {
				egLOG("resource/require", "Extention '{}' is not supported!", tag.extension());
				return std::shared_ptr<RESOURCE>{};
			}

			auto os_stream = require_resource_data(tag);
			auto resource_sp = adapter->second(tag, os_stream.get());
			if (resource_sp) {
				push_resource_to_cache(tag, resource_sp);
			}
			return std::static_pointer_cast<RESOURCE>(resource_sp);
		}

	private:
		resource_handle try_get_cached_resource(const tag& tag) {
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

		void push_resource_to_cache(const tag& tag, const resource_handle& resource) {
			std::lock_guard lock(cache_mutex);
			cache[tag] = resource;
		}

	public:
		memory_resolver memory_resolver_;

	private:
		std::unordered_map<protocol, resolver> resolvers;
		std::unordered_map<extension, adapter> adapters;
		std::unordered_map<tag, cache_entry> cache;
		std::unordered_map<res::tag, std::unordered_map<void*, reload_callback>> watchers;
		mutable std::mutex cache_mutex;
	};

	resource_system& get_system();
}
