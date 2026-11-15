#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>

namespace res
{
	// FAT and SMB keep file times to 2 s: a stamp within this of the time it was checked may hide a change made in the
	// same tick (a file saved twice, or replaced by one of the same size and time: cp -p, rsync; #973, #975).
	inline constexpr std::chrono::seconds k_stamp_granularity{ 2 };

	inline bool racy_stamp(std::filesystem::file_time_type stamp, std::filesystem::file_time_type checked_at)
	{
		return stamp >= checked_at - k_stamp_granularity;
	}

	// Ticks of std::filesystem::file_time_type, as the asset database keeps them.
	inline bool racy_stamp(std::int64_t stamp, std::int64_t checked_at)
	{
		using ticks = std::filesystem::file_time_type::duration;
		return racy_stamp(std::filesystem::file_time_type{ ticks{ stamp } },
			std::filesystem::file_time_type{ ticks{ checked_at } });
	}

	// The hash of a file's bytes as the asset database records it ("fnv1a64:<16 hex digits>"), fed in chunks.
	class content_hasher
	{
	public:
		void update(std::span<const std::byte> bytes)
		{
			for (const std::byte b : bytes) {
				m_value ^= static_cast<std::uint64_t>(b);
				m_value *= 1099511628211ull;
			}
		}

		std::string text() const { return std::format("fnv1a64:{:016x}", m_value); }

	private:
		std::uint64_t m_value = 14695981039346656037ull;
	};
}
