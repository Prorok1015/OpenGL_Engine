#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_base.h>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace core::res
{
	// Override deserialize with `deps` when the resource needs others, and collect_dependencies to name them.
	// Neither of them may wait for a resource: the pipeline loads the dependencies first and hands them in.
	class adapter_interface
	{
	public:
		virtual ~adapter_interface() = default;

		// Runs on a compute thread before deserialize. Empty by default.
		virtual std::vector<::res::res_dep> collect_dependencies(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const
		{
			(void)tag;
			(void)raw_data;
			return {};
		}

		// Runs once every dependency is ready. By default calls the overload without dependencies.
		virtual std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data, const ::res::res_deps& deps) const
		{
			(void)deps;
			return deserialize(tag, raw_data);
		}

		// For adapters without dependencies. An adapter overrides this one or the overload with `deps`.
		virtual std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const
		{
			(void)raw_data;
			throw std::logic_error("adapter does not implement deserialize for '" + std::string{ tag.view() } + "'");
		}

		virtual std::vector<std::byte> serialize(const ::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const = 0;
	};
}