#pragma once
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include "edt_model_importer.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edt {

	class process_decoder;

	// Where a model a tag names lies on the disk: the file, and the mount root (content root) it was found in.
	struct model_location
	{
		std::filesystem::path file;
		std::filesystem::path root;
	};

	// The editor's adapter for the model files descs refer to (res://x.glb as a level's prefab or a __parent): the model is
	// parsed in the decoder process, never in the editor (#925). It may read side files anywhere in its content root but
	// no hidden ones, each decoder step gets k_deadline, and without the decoder the load fails with its reason.
	class decoded_model_adapter : public core::res::adapter_interface
	{
	public:
		struct decoder_steps
		{
			std::function<model_file_scan(const import_source&, ds::task_context*)> scan;
			std::function<import_intermediate(const import_source&, ds::task_context*)> import;
			// Empty when the decoder can run.
			std::function<std::string()> unavailable_reason;
		};

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
		// "__models/<protocol>/<folder of the tag><stem>/": one memory:// folder per model, apart from the "<stem>/" of an
		// import. Each load empties it first: the decoder overwrites nothing, and memory:// is on the disk, so an earlier
		// run of the editor left the files of its load there.
		static std::string memory_folder_of(const res::tag& tag);

	private:
		desc::desc_system& m_descs;
		res::resource_system& m_resources;
		decoder_steps m_decoder;
	};

} // namespace edt
