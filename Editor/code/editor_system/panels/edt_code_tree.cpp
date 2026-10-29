#include "edt_code_tree.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_path_sanitize.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace edt
{
	namespace {

		std::string folded(const std::string& text)
		{
			std::string out = text;
			for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		std::string folder_of(std::string_view relative)
		{
			const std::size_t slash = relative.find_last_of('/');
			return slash == std::string_view::npos ? std::string{} : std::string{ relative.substr(0, slash) };
		}

		std::string join(std::string_view folder, std::string_view name)
		{
			return folder.empty() ? std::string{ name } : std::string{ folder } + "/" + std::string{ name };
		}

		// What Windows (and a shell) treat as more than a plain character; the portable rules of res cover the rest.
		std::string check_characters(std::string_view segment)
		{
			for (const char c : segment) {
				if (c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
					return "a name cannot hold any of \\ * ? \" < > |";
				}
			}
			return {};
		}

	}

	std::vector<code_entry> code_tree_model::list(std::string_view relative_dir) const
	{
		std::vector<code_entry> folders;
		std::vector<code_entry> files;
		std::error_code ec;
		const fs::path dir = relative_dir.empty() ? m_root : m_root / fs_paths::path_from_utf8(relative_dir);
		fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
		for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
			std::error_code entry_ec;
			const auto status = it->symlink_status(entry_ec);
			if (entry_ec) continue;
			const std::string name = fs_paths::utf8_of(it->path().filename());
			code_entry entry;
			entry.name = name;
			entry.relative = join(relative_dir, name);
			if (fs::is_directory(status)) {
				if (prj::is_code_ignored_dir(name)) continue;
				entry.is_directory = true;
				folders.push_back(std::move(entry));
			}
			else if (fs::is_regular_file(status)) {
				entry.category = prj::code_category_of(name);
				if (entry.category == prj::code_category::other) continue;
				files.push_back(std::move(entry));
			}
		}
		const auto by_name = [](const code_entry& a, const code_entry& b) { return folded(a.name) < folded(b.name); };
		std::ranges::sort(folders, by_name);
		std::ranges::sort(files, by_name);
		folders.insert(folders.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
		return folders;
	}

	fs::path code_tree_model::physical_path(std::string_view relative) const
	{
		return m_root / fs_paths::path_from_utf8(relative);
	}

	std::string check_code_file_path(std::string_view relative)
	{
		if (!relative.empty() && relative.front() == '/') return "the path must be relative to code/";
		if (std::string problem = res::portable_path_problem(relative); !problem.empty()) return problem;
		if (std::string problem = check_characters(relative); !problem.empty()) return problem;

		std::size_t begin = 0;
		std::string_view last;
		while (begin <= relative.size()) {
			const std::size_t end = relative.find('/', begin);
			last = relative.substr(begin, end == std::string_view::npos ? end : end - begin);
			if (end == std::string_view::npos) {
				break;
			}
			if (prj::is_code_ignored_dir(last)) {
				return std::string{ "'" } + std::string{ last } + "' is not a place for code (build output or a hidden folder)";
			}
			begin = end + 1;
		}
		if (!prj::is_code_file(last)) {
			return "only source, header and CMake files can be made here (.h .hpp .inl .cpp .cc .cxx .cmake, CMakeLists.txt, CMake*Presets.json)";
		}
		return {};
	}

	code_actions::code_actions(res::resource_system& res, code_tree_model tree, opener open_file)
		: m_res(res), m_tree(std::move(tree)), m_open_file(std::move(open_file))
	{
	}

	res::tag code_actions::tag_of(std::string_view relative)
	{
		return res::tag{ prj::k_code_protocol, relative };
	}

	res::file_operation code_actions::create_file(std::string_view folder, std::string_view name)
	{
		const std::string relative = join(folder, name);
		if (std::string problem = check_code_file_path(relative); !problem.empty()) {
			return res::file_operation::failure(std::move(problem));
		}
		const res::tag tag = tag_of(relative);
		auto result = m_res.create_sync(tag, {});
		if (result.ok) {
			egLOG("editor/code", "Created {}", tag.view());
		}
		return result;
	}

	res::file_operation code_actions::rename_file(std::string_view relative, std::string_view new_name)
	{
		if (new_name.find('/') != std::string_view::npos) {
			return res::file_operation::failure("a file is renamed within its folder: the name has no '/'");
		}
		const std::string target = join(folder_of(relative), new_name);
		if (std::string problem = check_code_file_path(target); !problem.empty()) {
			return res::file_operation::failure(std::move(problem));
		}
		return m_res.rename(tag_of(relative), tag_of(target));
	}

	res::file_operation code_actions::delete_file(std::string_view relative)
	{
		return m_res.remove(tag_of(relative));
	}

	res::file_operation code_actions::open_in_ide(std::string_view relative)
	{
		if (!m_open_file) {
			return res::file_operation::failure("opening files is not available here");
		}
		std::string error = m_open_file(m_tree.physical_path(relative));
		return error.empty() ? res::file_operation::success() : res::file_operation::failure(std::move(error));
	}
}
