#1108: scn::model_importer_adapter, scn::remember_model_imports and scn::model_importer_runs leave snk_engine (SNK_API) and
<snk/scene/adapters/scn_model_importer_adapter.h> is gone: the same adapter is mdl::model_adapter (with mdl::register_model_adapter,
mdl::remember_model_imports, mdl::model_importer_runs) in the new library snk_model_import_assimp (SNK_MODEL_IMPORT_API), which alone
links assimp; snk_engine no longer carries assimp. No type changes: the layout key stays. No game module uses them (tests/module_dll,
tests/editor_api_dll, tests/editor_module_dll and the project template name none of them); the game host registers mdl::model_adapter.
