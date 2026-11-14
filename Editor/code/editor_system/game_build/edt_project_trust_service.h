#pragma once
#include "edt_project_trust.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace res
{
	class resource_system;
}

namespace edt {

	class dialog_service;

	// What build.* and live mode answer a caller that is not the person while the project is not trusted (#921).
	inline constexpr std::string_view k_untrusted_project_error =
		"the project is not trusted: its build runs its own CMake and code";

	// The person's trust in the open project (#921) for the editor's session: what core::project_modules_options::
	// trust_library asks (main.cpp), what build.* and live mode check, and the modules waiting for the person's answer.
	// A standalone object in ds::app_data_storage, so no type a module sees changes. Main thread only.
	class project_trust_service
	{
	public:
		// `project_key`: edt::canonical_project_settings_key of the open project; nullopt (no project, or a path that
		// cannot be made canonical): nothing of build/bin is trusted and no answer is stored, unless `flag_trust`.
		project_trust_service(res::resource_system& res, std::optional<std::string> project_key,
			bool flag_trust = false);

		// --trust-project: everything loads and builds, for this run only; nothing is stored.
		void set_flag_trust(bool trusted) { m_flag = trusted; }
		[[nodiscard]] bool flag_trust() const { return m_flag; }
		[[nodiscard]] bool has_project() const { return m_key.has_value(); }

		// trust_library: whether the copy of `module` (in its own copy folder) may be loaded. A library the person
		// must be asked about is remembered as waiting and refused for now. Hashes only while there is no answer.
		bool allow_library(std::string_view module, const std::filesystem::path& copy);
		[[nodiscard]] bool waiting() const { return !m_waiting.empty(); }
		[[nodiscard]] const std::vector<std::string>& waiting_modules() const { return m_waiting; }
		// Before project_module_host::load_waiting, which asks allow_library again.
		void clear_waiting() { m_waiting.clear(); }
		// The modules allow_library refused, waiting or not, until it lets them load.
		[[nodiscard]] const std::vector<std::string>& held_modules() const { return m_held; }

		// Stores the person's answer (none: forgotten); a failed write is logged and the answer holds for this session.
		void answer(trust_answer answer);
		[[nodiscard]] trust_answer current_answer() const { return m_record.answer; }
		// The flag, or the person said "Trust".
		[[nodiscard]] bool trusted() const { return m_flag || m_record.answer == trust_answer::trusted; }

		// A library of build/bin passed this gate this session (it stays loaded whatever the answer is now).
		[[nodiscard]] bool allowed_any() const { return m_allowed_any; }

		// The editor built `library` as `module`: while the libraries of its folder digest so
		// (core::module_library_set_digest), a copy of it loads without the question. Not recorded when only the flag
		// trusts the project.
		void record_own_build(std::string_view module, const std::filesystem::path& library);
		void record_own_build_digest(std::string_view module, std::string_view digest);

		// ask_project_trust's dialog is shown and not answered yet.
		[[nodiscard]] bool question_open() const { return m_question_open; }
		void set_question_open(bool open) { m_question_open = open; }

	private:
		bool decide_library(std::string_view module, const std::filesystem::path& copy);

		res::resource_system& m_res;
		std::optional<std::string> m_key;
		bool m_flag = false;
		project_trust_record m_record;
		std::vector<std::string> m_waiting;
		std::vector<std::string> m_held;
		bool m_question_open = false;
		bool m_allowed_any = false;
	};

	// "a, b, c".
	std::string module_list(const std::vector<std::string>& modules);
	// The question's text: what loads and runs, the waiting modules, where the answer is kept.
	std::string project_trust_question(const std::vector<std::string>& waiting, trust_answer current);
	// The menu bar's line while modules are held back by trust; nullopt when none is.
	std::optional<std::string> held_modules_status(const std::vector<std::string>& held);

	// Asks the person "Trust this project?" through `dialogs`. "Trust" / "Don't Trust" are stored in `trust` and then
	// handed to `on_answer`; a dialog closed without an answer (Esc, headless auto-cancel) stores nothing and calls
	// nothing. While a question is open another call shows nothing and drops its `on_answer`. `trust` may be gone by
	// the answer.
	void ask_project_trust(dialog_service& dialogs, std::weak_ptr<project_trust_service> trust,
		std::function<void(trust_answer)> on_answer);

}
