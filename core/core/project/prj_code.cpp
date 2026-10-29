#include <snk/project/prj_code.h>
#include <algorithm>
#include <array>
#include <cctype>

namespace {

	constexpr std::array<std::string_view, 3> IGNORED_DIRS{ "build", "_deps", "CMakeFiles" };
	constexpr std::array<std::string_view, 3> SOURCE_EXTENSIONS{ ".cpp", ".cc", ".cxx" };
	constexpr std::array<std::string_view, 3> HEADER_EXTENSIONS{ ".h", ".hpp", ".inl" };
	constexpr std::string_view CMAKE_EXTENSION = ".cmake";

	std::string lowered(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	bool is_cmake_file_name(const std::string& name)
	{
		return name == "cmakelists.txt" || name == "cmakepresets.json" || name == "cmakeuserpresets.json";
	}

	bool ends_with(const std::string& text, std::string_view suffix)
	{
		return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

}

prj::code_category prj::code_category_of(std::string_view path)
{
	const std::size_t slash = path.find_last_of("/\\");
	const std::string name = lowered(slash == std::string_view::npos ? path : path.substr(slash + 1));

	if (is_cmake_file_name(name) || ends_with(name, CMAKE_EXTENSION)) return code_category::cmake;
	for (const auto extension : HEADER_EXTENSIONS) {
		if (ends_with(name, extension)) return code_category::header;
	}
	for (const auto extension : SOURCE_EXTENSIONS) {
		if (ends_with(name, extension)) return code_category::source;
	}
	return code_category::other;
}

std::span<const std::string_view> prj::code_ignored_dir_names()
{
	return IGNORED_DIRS;
}

bool prj::is_code_ignored_dir(std::string_view name)
{
	if (name.empty()) return false;
	if (name.front() == '.') return true;
	const std::string folded = lowered(name);
	return std::ranges::any_of(IGNORED_DIRS, [&folded](std::string_view ignored) { return lowered(ignored) == folded; });
}

std::vector<std::string> prj::code_write_patterns()
{
	std::vector<std::string> patterns;
	const auto allow = [&patterns](std::string_view file_glob) {
		patterns.push_back(std::string{ k_code_protocol } + "://**/" + std::string{ file_glob });
	};
	for (const auto extension : SOURCE_EXTENSIONS) allow(std::string{ "*" } + std::string{ extension });
	for (const auto extension : HEADER_EXTENSIONS) allow(std::string{ "*" } + std::string{ extension });
	allow(std::string{ "*" } + std::string{ CMAKE_EXTENSION });
	allow("CMakeLists.txt");
	allow("CMakePresets.json");
	allow("CMakeUserPresets.json");

	const auto deny = [&patterns](std::string_view folder_glob) {
		patterns.push_back(std::string{ "!" } + std::string{ k_code_protocol } + "://" + std::string{ folder_glob } + "/**");
		patterns.push_back(std::string{ "!" } + std::string{ k_code_protocol } + "://**/" + std::string{ folder_glob } + "/**");
	};
	for (const auto name : IGNORED_DIRS) deny(name);
	deny(".*");
	return patterns;
}
