#pragma once
#include "common.h"
#include "res_tag.h"
#include "logger/engine_log.h"
#include "res_resource_base.h"
#include "path_resolvers/res_memory_resolver.h"
#include <future>
#include <iostream>

namespace res
{
	class resource_system 
	{
	public:
		static std::string get_absolut_path(const tag& tag);
		using protocol = std::string_view;
		using extension = std::string_view;
		using data = std::vector<std::byte>;
		using resolver = std::function<std::future<data>(const tag&)>;
		using adapter = std::function<std::shared_ptr<Resource>(res::tag, const data&)>;

	public:
		resource_system();
		~resource_system() = default;
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		template<class RESOURCE>
		std::future<std::shared_ptr<RESOURCE>> require_resource_async(tag tag, bool hard_reload = false)
		{
			return std::async(std::launch::async, &resource_system::require_resource<RESOURCE>, this, tag, hard_reload);
		}

		template<class RESOURCE>
		std::shared_ptr<RESOURCE> require_resource(const tag& tag, bool hard_reload = false)
		{
			if (auto res = find_cache(tag)) {
				if (!hard_reload || tag.protocol() == tag::memory) {
					return std::static_pointer_cast<RESOURCE>(res);
				}

				cache_.erase(std::remove(cache_.begin(), cache_.end(), res));
			}

			auto res = std::make_shared<RESOURCE>(tag);
			cache_.push_back(res);
			return res;
		}

		void add_resource(std::shared_ptr<res::Resource> resource)
		{
			if (std::find(cache_.begin(), cache_.end(), resource) != cache_.end()) {
				egLOG("resource/add", "Resource '{}' already exist!", resource->tag_.get_full());
				return;
			}

			cache_.push_back(resource);
		}

		bool is_exist(const tag& tag) const {
			return find_cache(tag) != nullptr;
		}

		static std::filesystem::path get_resources_path() { return std::filesystem::path(s_res_path); }

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

		template<class RESOURCE>
		auto require_resource2(const res::tag& tag)
		{
			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return std::shared_ptr<RESOURCE>{};
			}

			if (adapters.find(tag.extension()) == adapters.end()) {
				egLOG("resource/require", "Extention '{}' is not supported!", tag.extension());
				return std::shared_ptr<RESOURCE>{};
			}

			auto os_stream = resolvers[tag.protocol()](tag);
			auto resource_sp = adapters[tag.extension()](tag, os_stream.get());
			return std::static_pointer_cast<RESOURCE>(resource_sp);
		}

	public:
		memory_resolver memory_resolver_;

	private:
		std::shared_ptr<Resource> find_cache(const tag& tag) const;

	private:
		std::vector<std::shared_ptr<Resource>> cache_;
		static std::string s_res_path;


		std::unordered_map<protocol, resolver> resolvers;
		std::unordered_map<extension, adapter> adapters;
		// resolvers_["res"] = std::bind(&resource_system::resolve_res, this, std::placeholders::_1);
		// resolvers_["memory"] = std::bind(&resource_system::resolve_memory, this, std::placeholders::_1);
		// 
		// tag = "res://a/b/c.d"
		// auto os_stream = resolvers_[tag.protocol()](tag);
		// std::unordered_map<able_extentions, loader> loaders_;
		// loaders_["asset"] = std::bind(&resource_system::load_asset, this, std::placeholders::_1);
		// auto resource_sp = loaders_[tag.extension()](tag, os_stream);

	};

	resource_system& get_system();
}
