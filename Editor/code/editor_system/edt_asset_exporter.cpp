#include "edt_asset_exporter.h"
#include <fstream>
#include <boost/json.hpp>

namespace json = boost::json;

edt::asset_exporter::asset_exporter(res::resource_system& res)
	: m_res(res)
{
}

std::unordered_map<res::tag, res::tag> edt::asset_exporter::build_remap(
	const import_result& result,
	const std::string& asset_name,
	const std::string& target_folder) const
{
	std::unordered_map<res::tag, res::tag> remap;
	std::string model_name = result.source_path.stem().string();

	for (const auto& mem_tag : result.all_tags) {
		std::string rel = std::string(mem_tag.relative());

		// Strip the model_name/ prefix from relative path
		std::string prefix = model_name + "/";
		if (rel.starts_with(prefix))
			rel = rel.substr(prefix.size());

		// Replace model name with asset name in the filename
		auto replace_stem = [&](std::string& path, const std::string& old_name, const std::string& new_name) {
			size_t pos = 0;
			while ((pos = path.find(old_name, pos)) != std::string::npos) {
				path.replace(pos, old_name.size(), new_name);
				pos += new_name.size();
			}
		};

		if (asset_name != model_name)
			replace_stem(rel, model_name, asset_name);

		std::string res_path = target_folder + "/" + rel;
		res::tag res_tag = res::tag::make(res_path);
		remap[mem_tag] = res_tag;
	}

	return remap;
}

void edt::asset_exporter::remap_json_strings(
	json::value& val,
	const std::unordered_map<res::tag, res::tag>& remap)
{
	if (val.is_string()) {
		std::string s(val.as_string().c_str(), val.as_string().size());
		// Check if this string looks like a memory:// tag
		if (s.starts_with("memory://")) {
			res::tag mem_tag{ s };
			if (auto it = remap.find(mem_tag); it != remap.end()) {
				val = std::string(it->second.view());
			}
		}
	} else if (val.is_object()) {
		for (auto& kv : val.as_object()) {
			remap_json_strings(kv.value(), remap);
		}
	} else if (val.is_array()) {
		for (auto& elem : val.as_array()) {
			remap_json_strings(elem, remap);
		}
	}
}

bool edt::asset_exporter::export_to_project(
	const import_result& result,
	const std::unordered_map<res::tag, res::tag>& remap) const
{
	auto base_path = res::resource_system::get_resources_path();

	// Process tags in order: leaves first (textures, geometry), then prefab last
	// Simple approach: prefab (.prefab.desc) goes last, everything else in order
	std::vector<res::tag> ordered_tags;
	res::tag prefab_tag;

	for (const auto& tag : result.all_tags) {
		std::string rel(tag.relative());
		if (rel.find(".prefab.desc") != std::string::npos)
			prefab_tag = tag;
		else
			ordered_tags.push_back(tag);
	}
	if (prefab_tag.is_valid())
		ordered_tags.push_back(prefab_tag);

	for (const auto& mem_tag : ordered_tags) {
		auto remap_it = remap.find(mem_tag);
		if (remap_it == remap.end()) {
			egLOG("edt/export", "No remap for tag '{}'", mem_tag.view());
			continue;
		}

		const res::tag& res_tag = remap_it->second;

		// Fetch bytes from memory://
		auto data_block = m_res.fetch_data(mem_tag);
		if (!data_block) {
			egLOG("edt/export", "Failed to fetch data for '{}'", mem_tag.view());
			continue;
		}

		auto& bytes = data_block->get();

		std::vector<std::byte> output_bytes;
		std::string rel(mem_tag.relative());

		// If it's a .desc file, parse JSON and remap memory:// references
		if (rel.find(".desc") != std::string::npos) {
			std::string json_str(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			try {
				json::value val = json::parse(json_str);
				remap_json_strings(val, remap);
				std::string remapped = json::serialize(val);
				output_bytes.resize(remapped.size());
				std::memcpy(output_bytes.data(), remapped.data(), remapped.size());
			} catch (const std::exception& e) {
				egLOG("edt/export", "Failed to parse JSON for '{}': {}", mem_tag.view(), e.what());
				output_bytes = bytes;
			}
		} else {
			output_bytes = bytes;
		}

		// Write to disk
		std::filesystem::path abs_path = base_path / std::string(res_tag.relative());
		std::error_code ec;
		std::filesystem::create_directories(abs_path.parent_path(), ec);
		if (ec) {
			egLOG("edt/export", "Failed to create directory '{}': {}", abs_path.parent_path().string(), ec.message());
			return false;
		}

		std::ofstream out(abs_path, std::ios::binary);
		if (!out) {
			egLOG("edt/export", "Failed to open file for writing: {}", abs_path.string());
			return false;
		}

		out.write(reinterpret_cast<const char*>(output_bytes.data()), output_bytes.size());
		out.close();

		// Register in VFS so the resource system can find it
		m_res.store(res_tag, output_bytes);

		egLOG("edt/export", "Exported: {} → {}", mem_tag.view(), abs_path.string());
	}

	egLOG("edt/export", "Export complete: {} files written", ordered_tags.size());
	return true;
}
