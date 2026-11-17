#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace edt {

	struct decoder_copy
	{
		std::filesystem::path program;
		std::filesystem::path folder;
	};

	// The decoder and the DLLs it loads from its own folder, copied into a folder an AppContainer can be given read and
	// execute on (#1054: a dev build's folder is not readable by containers, and its access list is the user's, never
	// changed). The DLLs are the ones `<decoder stem>.deps` beside it names (one file name a line, written by CMake) and
	// the C/C++ runtime DLLs beside it (vcruntime*, msvcp*, concrt*, ucrtbase*, clang_rt*), which the list does not hold.
	// The folder is <base>/<decoder stem>/<SHA-256 of every name and the SHA-256 of its bytes>: the same set is copied once;
	// a copy whose files differ is made again; folders of other sets of that decoder are removed. nullopt: `error` says why.
	std::optional<decoder_copy> make_decoder_copy(const std::filesystem::path& decoder, const std::filesystem::path& base,
		std::string& error);

	// Where the copies go: %LOCALAPPDATA%\snk\decoder on Windows; empty elsewhere or when the system does not say.
	std::filesystem::path decoder_copy_base();

	// A Windows exit code of a process that never ran its main: a DLL it could not find, read or initialize, an image it
	// could not load. A confined decoder that ends so could not load where it is (#1054).
	bool is_loader_failure(int exit_code);

} // namespace edt
