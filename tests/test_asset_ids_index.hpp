#pragma once
#include <snk/model_codec/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <snk/model_codec/res_tag.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace test_util
{
	// Path <-> ID both ways, as the editor's asset database answers.
	struct asset_ids_index final : res::asset_index
	{
		std::map<std::string, res::asset_id, std::less<>> ids;
		std::map<res::asset_id, res::tag> paths;

		void hold(std::string_view guid, std::string_view path)
		{
			const res::asset_id id = res::asset_id_from_text(guid).value();
			ids.emplace(std::string(path), id);
			paths.insert_or_assign(id, res::tag{ std::string(path) });
		}
		std::optional<res::asset_id> id_of(const res::tag& tag) const override
		{
			const auto found = ids.find(tag.view());
			return found == ids.end() ? std::nullopt : std::optional{ found->second };
		}
		std::optional<res::tag> path_of(const res::asset_id& id) const override
		{
			const auto found = paths.find(id);
			return found == paths.end() ? std::nullopt : std::optional{ found->second };
		}
	};

	// An index installed for a scope: a check that fails before the end of a test leaves none for the tests after it.
	class asset_index_scope
	{
	public:
		asset_index_scope(res::resource_system& resources, std::shared_ptr<const res::asset_index> index)
			: m_resources(resources)
		{
			m_resources.set_asset_index(std::move(index));
		}
		~asset_index_scope() { m_resources.set_asset_index(nullptr); }
		asset_index_scope(const asset_index_scope&) = delete;
		asset_index_scope& operator=(const asset_index_scope&) = delete;

	private:
		res::resource_system& m_resources;
	};

	// {"guid": ..., "path": ...} as a file writes a reference; an empty guid leaves the key out.
	inline std::string reference_text(std::string_view guid, std::string_view path)
	{
		std::string out = "{ ";
		if (!guid.empty()) {
			out += "\"guid\": \"" + std::string(guid) + "\", ";
		}
		return out + "\"path\": \"" + std::string(path) + "\" }";
	}
}
