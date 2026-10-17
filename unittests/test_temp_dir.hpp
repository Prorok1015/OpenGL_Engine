#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace test_util {

	// RAII scratch directory under the current working directory (the build dir when run via
	// ctest/unit_tests). Created empty on construction and removed on destruction, so tests never
	// leave files behind and never depend on state left by a previous run.
	class temp_dir
	{
	public:
		explicit temp_dir(std::string_view name)
			: m_path(std::filesystem::absolute(std::filesystem::path("unit_test_tmp") / std::string(name)).lexically_normal())
		{
			std::error_code ec;
			std::filesystem::remove_all(m_path, ec);
			std::filesystem::create_directories(m_path);
		}

		~temp_dir()
		{
			std::error_code ec;
			std::filesystem::remove_all(m_path, ec);
			// Drop the shared parent when it became empty (ignored otherwise).
			std::filesystem::remove(m_path.parent_path(), ec);
		}

		temp_dir(const temp_dir&) = delete;
		temp_dir& operator=(const temp_dir&) = delete;

		const std::filesystem::path& path() const { return m_path; }

		// Writes `content` to `<dir>/<rel>` (creating parent folders) and returns the full path.
		std::filesystem::path write(const std::filesystem::path& rel, std::string_view content) const
		{
			auto full = m_path / rel;
			std::filesystem::create_directories(full.parent_path());
			std::ofstream out(full, std::ios::binary | std::ios::trunc);
			out.write(content.data(), static_cast<std::streamsize>(content.size()));
			return full;
		}

	private:
		std::filesystem::path m_path;
	};

}
