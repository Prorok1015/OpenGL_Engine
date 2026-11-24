#pragma once
#include <snk/model_codec/res_tag.h>
#include <boost/json/array.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
	// appearance. Plain text: no include is followed and nothing is evaluated.
	std::vector<std::string> scan_define_names(std::string_view source);

	// The material window without ImGui (#1196): the define rows of a material_desc data document and the edits of its
	// `/defines`, each one record of the document's history. The rows are the union of the material's own defines, the
	// inherited ones and the names the stage files of its shader test, without the compiler's own (USE_ANIMATION, OPAQUE).
	// Main thread only: the shader and its stages are read with require_sync.
	class material_window_model
	{
	public:
		material_window_model(data_document& document, editor_services& services);

		// Sorted by name.
		std::vector<define_row> rows();
		// On when off, off when on (an inherited one is turned off by an own list without it). False when nothing changed.
		bool toggle(std::string_view name);
		// Turns `name` on; the name stays a row of the window, also once it is turned off. Empty when added, else why not.
		std::string add(std::string_view name);
		// Turns `name` off and forgets that it was added here. False when nothing changed.
		bool remove(std::string_view name);

		// The material's shader and its stage files, read again when the shader of the document changes.
		const material_shader& shader();
		// Reads the shader and its stages again at the next call (a stage file changed).
		void refresh_shader() { m_scanned = false; }

	private:
		bool write_defines(const std::function<void(boost::json::array& defines)>& change);
		void scan();

		data_document& m_document;
		editor_services& m_services;
		material_shader m_shader;
		std::vector<std::string> m_shader_names;
		std::vector<std::string> m_added;
		bool m_scanned = false;
	};
}
