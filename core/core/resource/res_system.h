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
	class resource_system 
	{
	public:
		static std::string get_absolut_path(const tag& tag);
		using protocol = std::string_view;
		using extension = adapter_info;
		using data = std::vector<std::byte>;
		using resolver = std::function<std::future<data>(const tag&)>;
		using adapter = std::function<std::shared_ptr<resource_entry>(res::tag, const data&)>;

	public:
		resource_system();
		~resource_system() = default;
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		static std::filesystem::path get_resources_path();

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

			return resolvers.at(tag.protocol())(tag);
		}

		template<class RESOURCE>
		auto require_resource(const res::tag& tag)
		{
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
			return std::static_pointer_cast<RESOURCE>(resource_sp);
		}

	public:
		memory_resolver memory_resolver_;

	private:
		std::unordered_map<protocol, resolver> resolvers;
		std::unordered_map<extension, adapter> adapters;
	};

	resource_system& get_system();
}
