#pragma once
#include <snk/model_codec/res_tag.h>
#include <snk/render/rnd_shader_problem.h>
#include <boost/json/array.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }
namespace rnd
{
	class shader_desc;
	namespace driver { class binding_table; }
}

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

	// A shader problem as the material window shows it (#1196 PR 3).
	struct compile_row
	{
		rnd::shader_problem problem;
		// The program had USE_ANIMATION: the skinned variant, which only a level's skinned mesh draws.
		bool skinned = false;
		// Found by the window's own preprocessing of the current define set (no GPU), not by a frame's build.
		bool preprocess = false;
		// The defines of the program(s) as their titles list them ("{ A | B }", "; " between variants); empty: none.
		std::string define_set;
	};

	struct compile_errors
	{
		// This material: its own load rows and the rows of its define set (base, opaque, skinned).
		std::vector<compile_row> material;
		// The same shader's files built with other define sets (other materials).
		std::vector<compile_row> others;
		// A sky material: there is no preview, the rows are those of the last frame the level drew.
		bool sky = false;
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
		// Reads the document's shader field and, when it or one of the files read changed, the shader and its stages. Free
		// when the document's generation is the same and no edit was pending: the shown document is not copied. Call it once
		// a frame before expected_defines, compile_rows, errors, pre_check and preprocess, which use what it read last.
		void scan();

		// The renderer's problems of this material and its shader (#1196 PR 3): load rows whose program is the document or
		// `preview_tag` and rows of the shader's files whose define set is the material's are `material`, the other
		// rows of those files `others`; the last preprocess() run's problems join `material`. Sets compare in any order.
		compile_errors compile_rows(const std::vector<rnd::shader_problem>& problems, const res::tag& preview_tag);
		// compile_rows, made again only when `generation` (render_system::shader_problems_generation), the document, the
		// shader files, a preprocess run or `preview_tag` changed; `snapshot` is asked only then.
		const compile_errors& errors(std::uint64_t generation, const std::function<std::vector<rnd::shader_problem>()>& snapshot,
			const res::tag& preview_tag);

		// What is likely wrong with the material's defines before anything compiles: an entry that is no identifier with
		// at most one value token, or a name the engine, the material compiler, the shader desc or the shader's source
		// defines itself. One message each. Made again when the document or the scan changed.
		const std::vector<std::string>& pre_check();

		static constexpr std::chrono::milliseconds preprocess_delay{ 300 };
		// Preprocesses the shader for the current define set (shader_config::load: includes, binding macros, no GPU)
		// once the set has stayed the same for preprocess_delay; a change restarts the wait. Call each frame with the
		// time. True when it ran.
		bool preprocess(std::chrono::steady_clock::time_point now, const rnd::driver::binding_table& bindings);

	private:
		// The entries of the shown `defines`, as the compiler gets them (with the shader's), sorted.
		const std::vector<std::string>& expected_defines();
		bool write_defines(const std::function<void(boost::json::array& defines)>& change);
		void sync_watches(const std::vector<res::tag>& wanted);
		void unwatch_files();

		data_document& m_document;
		editor_services& m_services;
		material_shader m_shader;
		std::shared_ptr<const rnd::shader_desc> m_program;
		// The stages and the files they include (not the shader desc).
		std::vector<res::tag> m_shader_files;
		// The names the stage sources #define themselves.
		std::vector<std::string> m_source_defines;
		std::uint64_t m_scan_serial = 0;
		std::vector<std::string> m_shader_names;
		std::vector<std::string> m_added;
		// The full entry ("NAME VALUE") of a define turned off here, by name.
		std::map<std::string, std::string, std::less<>> m_values;
		bool m_scanned = false;
		bool m_has_scan = false;
		// The document as the last scan read it: the same generation without a pending edit has the same shader field.
		std::uint64_t m_scan_generation = 0;
		bool m_scan_pending = false;
		// The resource system the watchers are on (the base one, which outlives the window) and what is watched.
		res::resource_system* m_watching = nullptr;
		std::vector<res::tag> m_watched;

		std::vector<std::string> m_expected;
		std::uint64_t m_expected_generation = 0;
		std::uint64_t m_expected_scan = 0;
		bool m_has_expected = false;

		std::vector<std::string> m_pre_check;
		std::uint64_t m_pre_check_generation = 0;
		std::uint64_t m_pre_check_scan = 0;
		bool m_has_pre_check = false;

		std::string m_preprocess_key;
		bool m_preprocess_seen = false;
		bool m_preprocess_pending = false;
		std::chrono::steady_clock::time_point m_preprocess_due{};
		std::vector<rnd::shader_problem> m_preprocessed;
		std::uint64_t m_preprocess_runs = 0;

		compile_errors m_errors;
		bool m_has_errors = false;
		std::uint64_t m_errors_generation = 0;
		std::uint64_t m_errors_document = 0;
		std::uint64_t m_errors_scan = 0;
		std::uint64_t m_errors_runs = 0;
		res::tag m_errors_preview;
	};
}
