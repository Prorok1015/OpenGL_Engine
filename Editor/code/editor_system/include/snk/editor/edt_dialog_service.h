#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	enum class unsaved_choice { save, discard, cancel };
	enum class message_level { info, warning, error };

	struct pick_file_options
	{
		std::string title;
		std::filesystem::path start_path;
		// With the leading dot, e.g. ".desc". Empty: every file.
		std::vector<std::string> extensions;
	};

	// Answers given without drawing anything; a dialog the answer does not cover is drawn as usual.
	struct dialog_auto_answer
	{
		std::optional<unsaved_choice> unsaved;
		std::optional<std::string> save_as_path;
		std::optional<std::filesystem::path> picked_file;
		// Dismiss every dialog with no answer (an unsaved prompt still reports `cancel`).
		bool cancel_all = false;
	};

	// Modal dialogs of the editor. They are queued and shown one at a time, in the order they were asked for;
	// each has its own id (also its ImGui popup id). The service draws inside a gui implicit callback, so the
	// ImGui context and popup id stack stay as they were. Main thread only.
	class dialog_service
	{
	public:
		using dialog_id = std::uint64_t;
		// Registers `draw` to be called every frame until it returns false (gui::menu_layout_manager::register_implicit).
		using implicit_registrar = std::function<void(std::string_view id, std::function<bool()> draw)>;

		struct request;

		dialog_service();
		~dialog_service();
		dialog_service(const dialog_service&) = delete;
		dialog_service& operator=(const dialog_service&) = delete;

		// Must be set before the first dialog is asked for.
		void set_registrar(implicit_registrar registrar) { m_registrar = std::move(registrar); }

		// "Save / Discard / Cancel" prompt. Reports `cancel` also when the popup is closed without a choice.
		dialog_id confirm_unsaved(std::function<void(unsaved_choice)> on_answer);
		// Asks for a path relative to res://. `on_path` runs once the popup is closed; cancelling reports nothing.
		dialog_id save_as(std::string initial_path, std::function<void(const std::string&)> on_path);
		// Own file dialog per call, so filters and start folder of one call never leak into another.
		dialog_id pick_file(pick_file_options options, std::function<void(const std::filesystem::path&)> on_pick);
		dialog_id message(message_level level, std::string text);

		void set_auto_answer(dialog_auto_answer answer) { m_auto_answer = std::move(answer); }
		void clear_auto_answer() { m_auto_answer.reset(); }

		std::size_t pending() const { return m_queue.size(); }
		// Asked for and not answered yet (queued or on screen).
		bool is_pending(dialog_id id) const;
		// The dialog on screen now.
		std::optional<dialog_id> current() const;

		// One frame: draws (or auto-answers) the front dialog only. Returns false once nothing is left.
		bool update();

	private:
		dialog_id enqueue(std::unique_ptr<request> req);

		implicit_registrar m_registrar;
		std::vector<std::unique_ptr<request>> m_queue;
		std::optional<dialog_auto_answer> m_auto_answer;
		dialog_id m_next_id = 1;
		std::uint64_t m_pump_serial = 0;
		bool m_pump_registered = false;
		// The registered callback holds a weak copy: it stops if the service is gone first.
		std::shared_ptr<void> m_alive = std::make_shared<int>(0);
	};
}
