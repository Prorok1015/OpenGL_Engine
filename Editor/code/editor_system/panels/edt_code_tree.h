#pragma once
#include <snk/project/prj_code.h>
#include <snk/resource/res_system.h>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	struct code_entry {
		std::string name;
		// Under code://, '/' separated, no leading '/'.
		std::string relative;
		bool is_directory = false;
		prj::code_category category = prj::code_category::other;
	};

	// GUI-free view of the project's code/ folder: code files and the folders that may hold them. Files of other kinds,
	// links, and the folders prj::is_code_ignored_dir names (build output, hidden folders) are left out, so the tree
	// shows what code:// may write and nothing else.
	class code_tree_model
	{
	public:
		explicit code_tree_model(std::filesystem::path root) : m_root(std::move(root)) {}

		// Folders first, then files, each by name without regard to case. A folder that is gone lists as empty.
		std::vector<code_entry> list(std::string_view relative_dir) const;

		const std::filesystem::path& root() const { return m_root; }
		std::filesystem::path physical_path(std::string_view relative) const;

	private:
		std::filesystem::path m_root;
	};

	// Empty when `relative` (under code://) names a place a code file may be made at: '/' separated names that every
	// platform can hold (res::portable_path_problem: no '..', ':', control characters, trailing space or dot, device
	// names), none of \ * ? " < > |, no ignored folder, and a code file name at the end. The write policy has the last word; this gives the user the reason before the attempt.
	std::string check_code_file_path(std::string_view relative);

	// What the Code tree does to files, through the resource system (the safe write of #427 and the write policy), so
	// the UI only draws. Every operation says why it did not happen.
	class code_actions
	{
	public:
		// Opens a file with the system's program; returns the error, empty on success.
		using opener = std::function<std::string(const std::filesystem::path&)>;

		code_actions(res::resource_system& res, code_tree_model tree, opener open_file);

		const code_tree_model& tree() const { return m_tree; }

		// A new empty file `name` (it may name subfolders, which are made) in `folder` ("" is code/). An existing file is
		// never overwritten.
		res::file_operation create_file(std::string_view folder, std::string_view name);
		// `new_name` is a plain file name: the file stays in its folder.
		res::file_operation rename_file(std::string_view relative, std::string_view new_name);
		res::file_operation delete_file(std::string_view relative);
		res::file_operation open_in_ide(std::string_view relative);

		static res::tag tag_of(std::string_view relative);

	private:
		res::resource_system& m_res;
		code_tree_model m_tree;
		opener m_open_file;
	};
}
