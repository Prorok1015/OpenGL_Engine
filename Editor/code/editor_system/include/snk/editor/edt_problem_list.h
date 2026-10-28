#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace edt
{
	class editor_events;

	enum class problem_severity { error, warning, note };

	const char* problem_severity_name(problem_severity severity);
	std::optional<problem_severity> problem_severity_from(std::string_view name);

	// One diagnostic: a compiler, linker or CMake message (build_output_parser), later also a desc error (#24).
	struct problem
	{
		problem_severity severity = problem_severity::error;
		// As printed; empty when the message names no file (linker, CMake without a location).
		std::string file;
		// 0: none.
		int line = 0;
		// 0: none.
		int column = 0;
		// "C2065", "LNK2019"; empty for GCC/Clang/CMake.
		std::string code;
		std::string message;
		// The note/context lines that belong to it, as printed.
		std::vector<std::string> notes;
	};

	// The source is not part of `problem`: the parser knows nothing of it, the list attaches it.
	struct sourced_problem
	{
		std::string source;
		problem item;
	};

	// The editor's problems, grouped by source ("build", later "desc"); each source replaces its own set as a whole.
	// Main thread only (the thread that created it), asserted by set() and clear(). Every change raises editor_events::problems_changed(source) and bumps generation().
	class problem_list
	{
	public:
		// `events` (null: none) must outlive the list.
		explicit problem_list(editor_events* events = nullptr);

		// Replaces the problems of `source`. A new source goes after the ones already set.
		void set(std::string_view source, std::vector<problem> items);
		// Forgets `source`; a later set() puts it last. Nothing happens for an unknown source.
		void clear(std::string_view source);

		// Sources in first-set order, the items of each in the order they were given.
		[[nodiscard]] std::vector<sourced_problem> list() const;
		[[nodiscard]] std::vector<std::string> sources() const;
		[[nodiscard]] std::size_t count(problem_severity severity) const;
		[[nodiscard]] std::size_t size() const;
		// Changes on every set() and on every clear() that removed something; lets a view rebuild only when needed.
		[[nodiscard]] std::uint64_t generation() const { return m_generation; }

	private:
		// By value: a slot may set() another source and move the stored names.
		void changed(std::string source);

		editor_events* m_events = nullptr;
		std::vector<std::pair<std::string, std::vector<problem>>> m_sources;
		std::uint64_t m_generation = 0;
		std::thread::id m_main_thread = std::this_thread::get_id();
	};
}
