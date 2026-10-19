#include <snk/editor/edt_dialog_service.h>
#include <snk/common/engine_assert.h>
#include "edt_file_dialog.h"
#include <imgui.h>
#include <algorithm>
#include <format>

struct edt::dialog_service::request
{
	explicit request(dialog_id request_id)
		: id(request_id)
	{
	}
	virtual ~request() = default;

	// Answers from `answer` when it covers this dialog; true if it did.
	virtual bool answer_automatically(const dialog_auto_answer& answer) = 0;
	// Draws one frame; true while the dialog stays open.
	virtual bool draw() = 0;

	std::string popup_label(const char* title) const { return std::format("{}##edt_dialog_{}", title, id); }

	dialog_id id;
};

namespace {

using dialog_id = edt::dialog_service::dialog_id;
using request = edt::dialog_service::request;

constexpr ImGuiWindowFlags MODAL_FLAGS = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;

class unsaved_request final : public request
{
public:
	unsaved_request(dialog_id request_id, std::function<void(edt::unsaved_choice)> on_answer)
		: request(request_id)
		, m_on_answer(std::move(on_answer))
	{
	}

	bool answer_automatically(const edt::dialog_auto_answer& answer) override
	{
		if (answer.unsaved) {
			finish(*answer.unsaved);
			return true;
		}
		if (answer.cancel_all) {
			finish(edt::unsaved_choice::cancel);
			return true;
		}
		return false;
	}

	bool draw() override
	{
		const std::string label = popup_label("Unsaved Changes");
		ImGui::OpenPopup(label.c_str());

		std::optional<edt::unsaved_choice> choice;
		if (ImGui::BeginPopupModal(label.c_str(), nullptr, MODAL_FLAGS)) {
			ImGui::Text("You have unsaved changes. What would you like to do?");
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			const float bw = 120.f;
			const float total = bw * 3.f + ImGui::GetStyle().ItemSpacing.x * 2.f;
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);

			if (ImGui::Button("Save & Continue", {bw, 0.f})) choice = edt::unsaved_choice::save;
			ImGui::SameLine();
			if (ImGui::Button("Discard", {bw, 0.f})) choice = edt::unsaved_choice::discard;
			ImGui::SameLine();
			if (ImGui::Button("Cancel", {bw, 0.f})) choice = edt::unsaved_choice::cancel;

			if (choice) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		} else {
			choice = edt::unsaved_choice::cancel;
		}

		// After the popup is closed, so the answer cannot touch ImGui state mid-modal.
		if (!choice) return true;
		finish(*choice);
		return false;
	}

private:
	void finish(edt::unsaved_choice choice)
	{
		if (m_on_answer) m_on_answer(choice);
	}

	std::function<void(edt::unsaved_choice)> m_on_answer;
};

class save_as_request final : public request
{
public:
	save_as_request(dialog_id request_id, std::string initial, std::function<void(const std::string&)> on_path)
		: request(request_id)
		, m_on_path(std::move(on_path))
	{
		auto cp = std::format_to_n(m_buffer, sizeof(m_buffer) - 1, "{}", initial);
		*cp.out = '\0';
	}

	bool answer_automatically(const edt::dialog_auto_answer& answer) override
	{
		if (answer.save_as_path) {
			finish(*answer.save_as_path);
			return true;
		}
		return answer.cancel_all;
	}

	bool draw() override
	{
		const std::string label = popup_label("Save Level");
		ImGui::OpenPopup(label.c_str());

		bool is_open = true;
		bool save = false;
		if (ImGui::BeginPopupModal(label.c_str(), nullptr, MODAL_FLAGS)) {
			ImGui::Text("Path (relative to res://):");
			ImGui::SetNextItemWidth(480.f);
			ImGui::InputText("##sp", m_buffer, sizeof(m_buffer));

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			const float bw = 110.f;
			const float avail = ImGui::GetContentRegionAvail().x;
			ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

			if (ImGui::Button("Save", {bw, 0.f})) {
				save = true;
				is_open = false;
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", {bw, 0.f})) {
				is_open = false;
			}

			if (!is_open) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		} else {
			is_open = false;
		}

		if (save) finish(m_buffer);
		return is_open;
	}

private:
	void finish(const std::string& path)
	{
		if (m_on_path) m_on_path(path);
	}

	char m_buffer[512] = {};
	std::function<void(const std::string&)> m_on_path;
};

class pick_file_request final : public request
{
public:
	pick_file_request(dialog_id request_id, edt::pick_file_options options,
		std::function<void(const std::filesystem::path&)> on_pick)
		: request(request_id)
		, m_options(std::move(options))
		, m_on_pick(std::move(on_pick))
	{
		m_dialog.set_current_path(m_options.start_path);
		m_dialog.set_select_mode(edt::file_dialog::SELECT_MODE::FILES_ONLY);
		for (const auto& ext : m_options.extensions)
			m_dialog.add_extension_filter(ext);
	}

	bool answer_automatically(const edt::dialog_auto_answer& answer) override
	{
		if (answer.picked_file) {
			if (m_on_pick) m_on_pick(*answer.picked_file);
			return true;
		}
		return answer.cancel_all;
	}

	bool draw() override
	{
		bool is_open = true;
		if (m_dialog.show(m_options.title.c_str(), &is_open)) {
			if (m_on_pick) m_on_pick(m_dialog.get_selected_path());
			return false;
		}
		return is_open;
	}

private:
	edt::pick_file_options m_options;
	std::function<void(const std::filesystem::path&)> m_on_pick;
	edt::file_dialog m_dialog;
};

class message_request final : public request
{
public:
	message_request(dialog_id request_id, edt::message_level level, std::string text)
		: request(request_id)
		, m_level(level)
		, m_text(std::move(text))
	{
	}

	// Nothing to answer: any auto answer simply acknowledges it.
	bool answer_automatically(const edt::dialog_auto_answer&) override { return true; }

	bool draw() override
	{
		const char* title = m_level == edt::message_level::error ? "Error"
			: m_level == edt::message_level::warning ? "Warning" : "Info";
		const std::string label = popup_label(title);
		ImGui::OpenPopup(label.c_str());

		bool is_open = true;
		if (ImGui::BeginPopupModal(label.c_str(), nullptr, MODAL_FLAGS)) {
			ImGui::TextUnformatted(m_text.c_str());
			ImGui::Spacing();
			if (ImGui::Button("OK", {120.f, 0.f})) {
				is_open = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		} else {
			is_open = false;
		}
		return is_open;
	}

private:
	edt::message_level m_level;
	std::string m_text;
};

} // namespace

edt::dialog_service::dialog_service() = default;

edt::dialog_service::~dialog_service()
{
	*m_alive = false;
}

edt::dialog_service::dialog_id edt::dialog_service::confirm_unsaved(std::function<void(unsaved_choice)> on_answer)
{
	return enqueue(std::make_unique<unsaved_request>(m_next_id, std::move(on_answer)));
}

edt::dialog_service::dialog_id edt::dialog_service::save_as(std::string initial_path,
	std::function<void(const std::string&)> on_path)
{
	return enqueue(std::make_unique<save_as_request>(m_next_id, std::move(initial_path), std::move(on_path)));
}

edt::dialog_service::dialog_id edt::dialog_service::pick_file(pick_file_options options,
	std::function<void(const std::filesystem::path&)> on_pick)
{
	return enqueue(std::make_unique<pick_file_request>(m_next_id, std::move(options), std::move(on_pick)));
}

edt::dialog_service::dialog_id edt::dialog_service::message(message_level level, std::string text)
{
	return enqueue(std::make_unique<message_request>(m_next_id, level, std::move(text)));
}

std::optional<edt::dialog_service::dialog_id> edt::dialog_service::current() const
{
	if (m_queue.empty()) return std::nullopt;
	return m_queue.front()->id;
}

edt::dialog_service::dialog_id edt::dialog_service::enqueue(std::unique_ptr<request> req)
{
	ASSERT_MSG(m_registrar, "dialog_service: no registrar set, call set_registrar() before asking for a dialog");
	const dialog_id id = req->id;
	++m_next_id;
	m_queue.push_back(std::move(req));
	if (!m_pump_registered) {
		m_pump_registered = true;
		// A fresh id per registration: the gui keeps a callback that just returned false until the end of the frame.
		m_registrar(std::format("edt/dialogs/{}", ++m_pump_serial),
			[this, alive = std::weak_ptr<bool>(m_alive)]() {
				const auto token = alive.lock();
				return token && *token && update();
			});
	}
	return id;
}

bool edt::dialog_service::update()
{
	if (m_queue.empty()) {
		m_pump_registered = false;
		return false;
	}

	// Callbacks may ask for more dialogs, which grows the queue; the request itself stays where it is.
	request& front = *m_queue.front();
	const bool answered = m_auto_answer && front.answer_automatically(*m_auto_answer);
	const bool open = !answered && front.draw();
	if (!open) {
		const auto done = std::find_if(m_queue.begin(), m_queue.end(),
			[&front](const auto& r) { return r.get() == &front; });
		m_queue.erase(done);
	}

	if (m_queue.empty()) {
		m_pump_registered = false;
		return false;
	}
	return true;
}
