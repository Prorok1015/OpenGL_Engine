#pragma once
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include "edt_model_importer.h"
#include "edt_process_decoder.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace edt {

	// Where a model a tag names lies on the disk: the file, and the mount root (content root) it was found in.
	struct model_location
	{
		std::filesystem::path file;
		std::filesystem::path root;
	};

	// The editor's adapter for the model files descs refer to (res://x.glb as a level's prefab or a __parent): the model is
	// parsed in the decoder process, never in the editor (#925). It may read side files anywhere in its content root but
	// no hidden ones, each decoder step gets k_deadline, and without the decoder the load fails with its reason. A model
	// that is no file on the disk (served from memory or a pack) is decoded from its bytes alone and may name no other file.
	// A model loaded again with the same bytes (and the same decoder, limits and place) is not decoded again (#999): its
	// earlier import is taken from its memory:// folder once that passes the checks of a decoder's answer.
	class decoded_model_adapter : public core::res::adapter_interface
	{
	public:
		using decoder_steps = edt::decoder_steps;

		// Shorter than an agent's import: a level waits for its models.
		static constexpr std::chrono::seconds k_deadline{ 10 };

		decoded_model_adapter(desc::desc_system& descs, res::resource_system& resources, decoder_steps decoder);
		// The steps of `decoder`, which the adapter keeps alive (a load can outlive the editor_system that made it).
		static decoder_steps steps_of(std::shared_ptr<const process_decoder> decoder);

		std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;

		// The file of `tag` in the highest of `roots` (lowest priority first, as resource_system::mount_roots lists them)
		// that holds it; nullopt when none does.
		static std::optional<model_location> locate(const std::vector<std::filesystem::path>& roots, const res::tag& tag);
		// "__models/<protocol>/<folder of the tag><file name>/": one memory:// folder per model file (x.glb and x.fbx apart),
		// apart from the "<stem>/" of an import. Each load empties it first: the decoder overwrites nothing, and memory://
		// is on the disk, so an earlier run of the editor left the files of its load there.
		static std::string memory_folder_of(const res::tag& tag);

	private:
		// The last decode of a model's memory:// folder, for this run of the editor only.
		struct cached_decode
		{
			std::string key;
			model_file_scan scan;
			import_intermediate imported;
		};

		// The earlier import of `source`'s folder when its key is `key`, its side files still have the bytes the scan read,
		// and what is in memory:// passes accept_import_answer; nullopt (with `problem` when it was refused) otherwise.
		std::optional<import_intermediate> cached(const std::string& key, const import_source& source, std::string& problem) const;
		void remember(const std::string& folder, cached_decode decode) const;
		void forget(const std::string& folder) const;

		// Held from emptying a model's folder until its import is in: two loads of one model do not erase each other.
		std::shared_ptr<std::mutex> folder_lock(const std::string& folder) const;

		desc::desc_system& m_descs;
		res::resource_system& m_resources;
		decoder_steps m_decoder;
		mutable std::mutex m_folder_locks_mutex;
		mutable std::unordered_map<std::string, std::shared_ptr<std::mutex>> m_folder_locks;
		mutable std::mutex m_cache_mutex;
		mutable std::unordered_map<std::string, cached_decode> m_cache;
	};

} // namespace edt
