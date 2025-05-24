#pragma once
#include "common.h"
#include "res_system.h"
#include "res_tag.h"
#include <assimp/IOSystem.hpp>

namespace scn
{
    // Fisher Price - My First Filesystem
    class engine_assimp_resource_system_wrapper : public Assimp::IOSystem {
    public:
        engine_assimp_resource_system_wrapper(res::resource_system& rs, res::tag source, const std::vector<std::byte>& d)
			: source_tag(source), data(d), resource_system(rs) {
		}

        ~engine_assimp_resource_system_wrapper() = default;

        // Check whether a specific file exists
        virtual bool Exists(const char* pFile) const override {
            return true;
        }

        // Get the path delimiter character we'd like to see
        virtual char getOsSeparator() const override {
            return '/';
        }

        virtual Assimp::IOStream* Open(const char* pFile, const char* pMode) override;

        virtual void Close(Assimp::IOStream* pFile) override { delete pFile; }
    private:
        res::tag source_tag;
		const std::vector<std::byte>& data;
		std::unordered_map<res::tag, std::vector<std::byte>> data_map;
		res::resource_system& resource_system;
    };
}