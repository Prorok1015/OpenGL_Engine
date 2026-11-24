#pragma once
#include <snk/model_codec/res_tag.h>
#include <boost/json/array.hpp>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }

namespace edt
{
	class data_document;
	struct editor_services;

	// Where a define of the material window is on (#1196).
	enum class define_state
	{
		own_on,       // the material's own `defines` set it
		inherited_on, // the file has no `defines` of its own and a parent sets it
		off,
	};

	struct define_row
	{
		std::string name;
		define_state state = define_state::off;
		// A parent of the material sets it: shown as inherited even when this file turns it off.
		bool inherited = false;
		// A stage file of the material's shader tests it (#ifdef, #ifndef, defined(...)).
		bool from_shader = false;
	};

	// The shader of the material and its stage files (empty tags: none or not known).
	struct material_shader
	{
		res::tag shader;
		res::tag vertex;
		res::tag fragment;
		res::tag geometry;
		// Why the shader or one of its stages could not be read; empty: none.
		std::string error;
	};

	// The defines the material compiler sets on its own: a toggle of them would do nothing.
	bool is_compiler_define(std::string_view name);
	// A preprocessor identifier: [A-Za-z_][A-Za-z0-9_]*.
	bool is_define_name(std::string_view name);
	// The names a GLSL source tests: #ifdef X, #ifndef X and defined(X) / defined X in #if and #elif, each once, in order of
	// appearance, without the comments and without the names the source #defines itself (include guards, platform macros...). No
	// include is followed (the caller expands them) and nothing is evaluated.
	std::vector<std::string> scan_define_names(std::string_view source);

	// The material window without ImGui (#1196): the define rows of a material_desc data document and the edits of its
	// `/defines`, each one record of the document's history. The rows are the union of the material's own defines, the
	// inherited ones and the names the stage files of its shader test, without the compiler's own (USE_ANIMATION, OPAQUE).
	// Main thread only: the shader and its stages are read with require_sync.
	class material_window_model
	{
	public:
		material_window_model(data_document& document, editor_services& services);
		~material_window_model();
		material_window_model(const material_window_model&) = delete;
		material_window_model& operator=(const material_window_model&) = delete;

		// Sorted by name.
		std::vector<define_row> rows();
		// On when off, off when on (an inherited one is turned off by an own list without it). False when nothing changed.
		// A list that ends up with the same entries as the parents' is no own list: the material follows its parents again.
		// A define turned off with a value ("NAME 2") gets the value back when it is turned on in the same session.
		bool toggle(std::string_view name);
		// Turns `name` on; the name stays a row of the window, also once it is turned off. Empty when added, else why not.
		std::string add(std::string_view name);
		// Turns `name` off and forgets that it was added here. False when nothing changed.
		bool remove(std::string_view name);
		// The material has a parent and an own `defines`: reset_to_inherited would drop it.
		bool can_reset_to_inherited() const;
		// Drops the own `defines` (the mark's Reset of the field): one record of the history. False when nothing changed.
		bool reset_to_inherited();

		// The material's shader and its stage files (their includes expanded), read again when the committed shader of the
		// document changes or one of the files read is signalled as changed.
		const material_shader& shader();
		// Reads the shader and its stages again at the next call.
		void refresh_shader() { m_scanned = false; }

	private:
		bool write_defines(const std::function<void(boost::json::array& defines)>& change);
		void scan();
		void sync_watches(const std::vector<res::tag>& wanted);
		void unwatch_files();

		data_document& m_document;
		editor_services& m_services;
		material_shader m_shader;
		std::vector<std::string> m_shader_names;
		std::vector<std::string> m_added;
		// The full entry ("NAME VALUE") of a define turned off here, by name.
		std::map<std::string, std::string, std::less<>> m_values;
		bool m_scanned = false;
		bool m_has_scan = false;
		// The resource system the watchers are on (the base one, which outlives the window) and what is watched.
		res::resource_system* m_watching = nullptr;
		std::vector<res::tag> m_watched;
	};
}
