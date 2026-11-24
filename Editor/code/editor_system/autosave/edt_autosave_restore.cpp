#include "edt_autosave_restore.h"
#include "edt_data_document.h"
#include "edt_mesh_document.h"
#include "edt_mesh_io.h"
#include "edt_prefab_document.h"
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	std::optional<boost::json::object> object_of(std::span<const std::byte> bytes, std::string& error)
	{
		boost::json::error_code ec;
		boost::json::parse_options options;
		options.max_depth = 256;
		boost::json::value parsed = boost::json::parse(
			std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec, {}, options);
		if (ec || !parsed.is_object()) {
			error = ec ? std::format("the autosave is no JSON object: {}", ec.message()) : std::string{ "the autosave is no JSON object" };
			return std::nullopt;
		}
		return std::move(parsed.as_object());
	}
}

std::string edt::restore_prefab_bytes(res::resource_system& writable, res::resource_system& resources, prefab_document& prefab,
	const res::tag& copy, std::span<const std::byte> bytes)
{
	if (!prefab.is_open()) return "the prefab is not open";
	if (!writable.store_sync(copy, std::vector<std::byte>(bytes.begin(), bytes.end()))) return "the working copy could not be stored";
	std::string error;
	{
		auto handle = resources.require_sync<scn::prefab_desc>(copy);
		const std::shared_ptr<scn::prefab_desc> read = handle.is_ready() ? handle.get_sync() : nullptr;
		if (!read) {
			error = "the autosave does not read as a prefab";
		} else if (!prefab.edit([&read](scn::prefab_desc& working) {
				read->copy_to(working);
				return true;
			})) {
			error = "the prefab cannot be edited now";
		}
	}
	static_cast<void>(writable.erase(copy));
	return error;
}

std::string edt::restore_mesh_bytes(mesh_document& mesh, std::span<const std::byte> bytes)
{
	if (!mesh.is_open()) return "the mesh is not open";
	std::string error;
	const std::optional<boost::json::object> object = object_of(bytes, error);
	if (!object) return error;
	mesh::poly_mesh_desc read;
	try {
		mesh::poly_mesh_from_json(*object, read);
	} catch (const std::exception& failure) {
		return std::format("the autosave does not read as a mesh: {}", failure.what());
	}
	if (!mesh.edit([&read](mesh::poly_mesh& working) {
			working = std::move(read.mesh);
			return true;
		})) {
		return "the mesh cannot be edited now";
	}
	return {};
}

std::string edt::restore_data_bytes(data_document& data, std::span<const std::byte> bytes)
{
	if (!data.is_open()) return "the document is not open";
	std::string error;
	std::optional<boost::json::object> object = object_of(bytes, error);
	if (!object) return error;
	if (!data.restore_own(std::move(*object))) return "the autosave is of another __type or __parent than the file";
	return {};
}
