#pragma once
#include "desc_system.h"
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace desc
{
	constexpr uint64_t invalid_node_id = 0;

	// Random id for a node created by a user action. Never returns invalid_node_id.
	inline uint64_t new_node_id()
	{
		thread_local std::mt19937_64 rng = [] {
			std::random_device rd;
			std::seed_seq seq{ rd(), rd(), rd(), rd() };
			return std::mt19937_64{ seq };
		}();
		uint64_t id = invalid_node_id;
		while (id == invalid_node_id) {
			id = rng();
		}
		return id;
	}

	// Stable id for a node addressed by its name path ("ROOT/Armature/geometry_0#1"): the same path always
	// yields the same id, so files and importers that carry no explicit id get ids that survive reloads.
	inline uint64_t derive_node_id(std::string_view name_path)
	{
		const uint64_t id = desc_system::fnv1a_64(name_path);
		return id == invalid_node_id ? 1 : id;
	}

	// 16-char lowercase hex; JSON numbers above 2^53 are not safe in external tools.
	inline std::string format_node_id(uint64_t id)
	{
		return std::format("{:016x}", id);
	}

	inline std::optional<uint64_t> parse_node_id(std::string_view text)
	{
		if (text.size() != 16) {
			return std::nullopt;
		}
		uint64_t id = 0;
		auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id, 16);
		if (ec != std::errc{} || end != text.data() + text.size() || id == invalid_node_id) {
			return std::nullopt;
		}
		return id;
	}
}
