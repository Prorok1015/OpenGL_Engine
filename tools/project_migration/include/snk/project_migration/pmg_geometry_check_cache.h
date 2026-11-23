#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace prj::migration
{
	// What geometry_conversion_problem said of a desc, kept between the scans of one editor session (#1155): the check
	// encodes the whole pair, and a project not yet migrated is scanned on every open and every plan. The answer depends
	// on the path and the JSON of the desc alone, so the key is the path with the size and SHA-256 of the text: a file
	// touched without a change keeps its answer, any change of its bytes is checked again. One answer a path is kept; past
	// `max_entries` paths a new one is checked every time and the ones kept stay.
	class geometry_check_cache
	{
	public:
		static constexpr std::size_t default_max_entries = 4096;

		explicit geometry_check_cache(std::size_t max_entries = default_max_entries);

		// The remembered answer for this text at this path, else what `check` gives (then remembered). `check` runs
		// without the lock held: two scans may both run it for the same text, and either answer is the same.
		std::string problem(std::string_view path, std::string_view text, const std::function<std::string()>& check);

		void clear();
		std::size_t size() const;

	private:
		struct key
		{
			std::string path;
			std::size_t size = 0;
			std::string sha256;

			auto operator<=>(const key&) const = default;
		};

		mutable std::mutex m_mutex;
		std::map<key, std::string> m_problems;
		std::size_t m_max_entries;
	};
}
