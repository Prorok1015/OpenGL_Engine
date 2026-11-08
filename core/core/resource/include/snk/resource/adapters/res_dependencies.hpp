#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_base.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/common/ds/ds_type_id.hpp>
#include <snk/common/ds/ds_polymorphic_cast.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace res
{
	class resource_system;

	// A resource an adapter needs before it can deserialize. Build it with res::make_dep<T>(tag) (res_system.h).
	struct res_dep
	{
		::res::tag target;
		ds::type_id type;
		// Starts (or joins) the load of `target` as `type` without waiting; what make_dep<T> binds to require<T>.
		std::shared_ptr<core::res::res_resource_block> (*request)(resource_system&, const ::res::tag&) = nullptr;
		// How the dependent's failure is reported when this one fails (#595): a res::diagnostic_kind and where the
		// dependent names it. Empty kind: diagnostic_kind::load.
		std::string_view failure_kind{};
		std::string json_path{};
	};

	// The dependencies of one deserialize call, all ready, in the order collect_dependencies returned them.
	class res_deps
	{
	public:
		struct entry
		{
			res_dep dep;
			std::shared_ptr<resource_entry> value;
		};

		void add(res_dep dep, std::shared_ptr<resource_entry> value)
		{
			m_entries.push_back(entry{ std::move(dep), std::move(value) });
		}

		// Null also when the dependency loaded as something that is not a T (#615).
		template<class T>
		std::shared_ptr<T> get(const ::res::tag& target) const
		{
			const auto type = ds::type_id::make<T>();
			for (const auto& item : m_entries) {
				if (item.dep.type == type && item.dep.target == target) {
					return std::dynamic_pointer_cast<T>(item.value);
				}
			}
			return nullptr;
		}

		const std::vector<entry>& entries() const { return m_entries; }
		bool empty() const { return m_entries.empty(); }
		std::size_t size() const { return m_entries.size(); }

	private:
		std::vector<entry> m_entries;
	};
}
