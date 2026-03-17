#pragma once
#include "common.h"
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_file_dialog.h"

namespace edt {

	class asset_export_dialog {
	public:
		asset_export_dialog(asset_exporter& exporter);

		// Set the import result to export
		void open(import_result result);

		// Render the dialog. Returns false when dialog is closed.
		bool render();

		// After successful export, the res:// tag of the root prefab
		res::tag get_exported_tag() const { return m_exported_tag; }

	private:
		asset_exporter& m_exporter;

		bool m_is_open = false;
		bool m_needs_open = false;
		import_result m_import;
		char m_asset_name[256] = {};
		char m_target_folder[512] = {};

		bool m_browse_folder = false;
		file_dialog m_folder_dialog;

		res::tag m_exported_tag;
	};

} // namespace edt
