#pragma once
// A copy of a test project from tests/data/projects (SNK_TEST_FIXTURES_DIR) in a fresh temporary folder, removed with the
// object: a test may migrate or rewrite it, the fixture in the repository stays as it is.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace test_util {

inline std::filesystem::path fixture_source(std::string_view name)
{
	return std::filesystem::path{ SNK_TEST_FIXTURES_DIR } / std::string{ name };
}

class fixture_copy
{
public:
	explicit fixture_copy(std::string_view name)
	{
		static std::atomic<int> counter{ 0 };
		m_root = std::filesystem::temp_directory_path()
			/ std::format("snk_fixture_{}_{}_{}", name, std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
		std::filesystem::create_directories(m_root);
		std::filesystem::copy(fixture_source(name), m_root, std::filesystem::copy_options::recursive);
	}

	~fixture_copy()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}

	fixture_copy(const fixture_copy&) = delete;
	fixture_copy& operator=(const fixture_copy&) = delete;

	const std::filesystem::path& root() const { return m_root; }
	std::filesystem::path res() const { return m_root / "res"; }

private:
	std::filesystem::path m_root;
};

}
