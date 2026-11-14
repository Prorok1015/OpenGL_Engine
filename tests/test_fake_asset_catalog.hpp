#pragma once
#include "edt_asset_picker.h"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace test_util
{
	namespace picker = edt::picker;

	// A list of asset records standing for the asset database, for the tests of the reference picker.
	struct fake_catalog final : picker::catalog
	{
		bool ready() const override { return is_ready; }
		void for_each(const std::function<void(const edt::asset_record&)>& visit) const override
		{
			++walks;
			for (const edt::asset_record& record : records) {
				visit(record);
			}
		}
		const edt::asset_record* find(const res::tag& path) const override
		{
			const auto found = std::ranges::find_if(records, [&](const edt::asset_record& record) { return record.path.relative() == path.relative(); });
			return found == records.end() ? nullptr : &*found;
		}
		std::size_t layer_count() const override { return layers; }
		std::uint64_t revision() const override { return version; }
		bool settled() const override { return is_settled; }

		void add(const std::string& path, std::size_t layer, const std::string& type, const char* desc_type = nullptr, bool hashed = true)
		{
			edt::asset_record record;
			record.path = res::tag::make(path);
			record.layer = layer;
			record.type = type;
			if (desc_type) {
				record.desc_type = desc_type;
			}
			if (hashed) {
				record.content_hash = "fnv1a64:0000000000000001";
			}
			records.push_back(std::move(record));
		}

		std::vector<edt::asset_record> records;
		mutable int walks = 0;
		std::size_t layers = 3;
		bool is_ready = true;
		bool is_settled = true;
		std::uint64_t version = 1;
	};

}
