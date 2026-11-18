#pragma once
#include "edt_model_importer.h"
#include "edt_process_decoder.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/resource/res_system.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace edt {

	// File > Import Model (#987), in a job: reads `path` (at most k_max_person_source_bytes), lists the files the model
	// reads besides itself and imports it with exactly those, each step in the decoder within k_person_parse_time_limit,
	// into memory://`memory_folder` (emptied first). As for an agent, the model may read files of its own folder and its
	// non-hidden subfolders only. The model is never parsed in this process: without the decoder the import fails with the
	// reason. A failure is `error`, naming the step and the size of the file; a stop request throws ds::task_cancelled, and
	// one that arrives while the decoder imports erases what it stored first.
	import_intermediate person_model_import(const std::filesystem::path& path, const std::string& memory_folder,
		const decoder_steps& decoder, res::resource_system& resources, ds::task_context& ctx);

	// "__imports/<process id>/<serial>/<stem>/": one memory:// folder per import a person starts, apart from an agent's
	// "<stem>/" and the "__models/" of the models levels refer to. memory:// is shared by every editor on the project, so
	// each run has a folder of its own.
	std::string person_import_folder(const std::filesystem::path& path, std::int64_t process_id, std::uint64_t serial);

	// At startup: erases the "__imports/<process id>/" folders of memory:// whose process is not running (a run that crashed
	// or was killed), never `current_process_id`'s; a folder whose name is no process id is left as it is.
	void erase_imports_of_ended_runs(res::resource_system& resources, std::int64_t current_process_id);

} // namespace edt
