#pragma once
#include <snk/model_import/mdl_import.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <string>
#include <utility>

namespace mdl::detail
{
	// Assimp reads the model and the files it names through a file_source. Not exported: assimp stays inside this library.
	class source_io final : public Assimp::IOSystem
	{
	public:
		source_io(file_source& files, std::string display_name) : m_files(files), m_display_name(std::move(display_name)) {}

		bool Exists(const char* file) const override;
		char getOsSeparator() const override { return '/'; }
		Assimp::IOStream* Open(const char* file, const char* mode) override;
		void Close(Assimp::IOStream* file) override;

	private:
		file_source& m_files;
		std::string m_display_name;
	};
}
