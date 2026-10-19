#include <snk/editor/edt_dialog_service.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include "edt_file_dialog.h"
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <imgui.h>
#include <algorithm>
#include <format>

namespace json = boost::json;

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

	// What the dialog is, for dialog_service::list().
	virtual const char* kind() const = 0;
	virtual std::string text() const = 0;
	virtual json::value answer_schema() const = 0;
	// Stores an answer that already matched answer_schema, to be acted on like the click it stands for. An error
	// text if the answer means nothing for this dialog.
	virtual std::optional<std::string> accept(const json::value& answer) = 0;
	// Acts on the accepted answer of a dialog that was never drawn: the callback a click would have called.
	virtual void settle() = 0;

	std::string popup_label(const char* title) const { return std::format("{}##edt_dialog_{}", title, id); }

	// Opens the popup on the first frame only: one closed from outside (Esc) must stay closed, and
	// BeginPopupModal returning false afterwards is the request's cancel.
	bool begin_modal(const std::string& label, int flags)
	{
		if (!m_opened) {
			ImGui::OpenPopup(label.c_str());
			m_opened = true;
		}
		return ImGui::BeginPopupModal(label.c_str(), nullptr, flags);
	}

	dialog_id id;
	// Put on screen by update() at least once.
	bool drawn = false;
	// An answer was accepted; the next draw() acts on it (see dialog_service::answer).
	bool answered = false;

private:
	bool m_opened = false;
};

namespace {

using dialog_id = edt::dialog_service::dialog_id;
using request = edt::dialog_service::request;

constexpr ImGuiWindowFlags MODAL_FLAGS = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;

// The answer of a dialog that asks for a path: the path, or a cancel.
struct path_answer
{
	bool cancel = false;
	std::string path;
};

std::optional<std::string> read_path_answer(const json::value& answer, std::optional<path_answer>& out)
{
	const json::object& fields = answer.as_object();
	const json::value* path = fields.if_contains("path");
	const json::value* cancel = fields.if_contains("cancel");
	const bool wants_cancel = cancel && cancel->is_bool() && cancel->as_bool();
	if (wants_cancel && path) return "give either a path or cancel, not both";
	if (wants_cancel) {
		out = path_answer{ true, {} };
		return std::nullopt;
	}
	if (path && path->is_string() && !path->as_string().empty()) {
		out = path_answer{ false, std::string(path->as_string()) };
		return std::nullopt;
	}
	return "the answer needs a path, or cancel: true";
}

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

		std::optional<edt::unsaved_choice> choice;
		if (begin_modal(label, MODAL_FLAGS)) {
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
			// Answered from outside (dialog_service::answer): the same as a click on that button.
			if (!choice) choice = m_answer;

			if (choice) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		} else {
			choice = m_answer.value_or(edt::unsaved_choice::cancel);
		}

		// After the popup is closed, so the answer cannot touch ImGui state mid-modal.
		if (!choice) return true;
		finish(*choice);
		return false;
	}

	const char* kind() const override { return "unsaved"; }
	std::string text() const override { return "You have unsaved changes. What would you like to do?"; }

	json::value answer_schema() const override
	{
		return json::parse(R"({"type":"object","properties":{"choice":{"type":"string","enum":["save","discard","cancel"]}},"required":["choice"],"additionalProperties":false})");
	}

	std::optional<std::string> accept(const json::value& answer) override
	{
		const json::value* choice = answer.as_object().if_contains("choice");
		if (choice && choice->is_string()) {
			const json::string& name = choice->as_string();
			if (name == "save") m_answer = edt::unsaved_choice::save;
			if (name == "discard") m_answer = edt::unsaved_choice::discard;
			if (name == "cancel") m_answer = edt::unsaved_choice::cancel;
		}
		if (!m_answer) return "choice must be save, discard or cancel";
		return std::nullopt;
	}

	void settle() override
	{
		if (m_answer) finish(*m_answer);
	}

private:
	void finish(edt::unsaved_choice choice)
	{
		if (m_on_answer) m_on_answer(choice);
	}

	std::function<void(edt::unsaved_choice)> m_on_answer;
	std::optional<edt::unsaved_choice> m_answer;
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

		bool is_open = true;
		bool save = false;
		bool from_answer = false;
		if (begin_modal(label, MODAL_FLAGS)) {
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
			// Answered from outside (dialog_service::answer): the same as a click on Save or Cancel.
			if (is_open && m_answer) {
				save = !m_answer->cancel;
				from_answer = true;
				is_open = false;
			}

			if (!is_open) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		} else {
			is_open = false;
		}

		// A click in the same frame wins over the answer, as for every other kind. The path of an answer is used as
		// given; the text buffer would cut a long one.
		if (save) finish(from_answer ? m_answer->path : std::string(m_buffer));
		return is_open;
	}

	const char* kind() const override { return "save_as"; }
	std::string text() const override { return "Path (relative to res://):"; }

	json::value answer_schema() const override
	{
		return json::parse(R"({"type":"object","properties":{"path":{"type":"string","minLength":1},"cancel":{"type":"boolean"}},"minProperties":1,"additionalProperties":false})");
	}

	std::optional<std::string> accept(const json::value& answer) override
	{
		return read_path_answer(answer, m_answer);
	}

	void settle() override
	{
		if (m_answer && !m_answer->cancel) finish(m_answer->path);
	}

private:
	void finish(const std::string& path)
	{
		if (m_on_path) m_on_path(path);
	}

	char m_buffer[512] = {};
	std::function<void(const std::string&)> m_on_path;
	std::optional<path_answer> m_answer;
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
		// Answered from outside (dialog_service::answer): the dialog closes itself inside its popup, as on OK or Cancel.
		if (m_answer) m_dialog.request_close();
		// A click on OK in the same frame wins over the answer.
		if (m_dialog.show(m_options.title.c_str(), &is_open)) {
			if (m_on_pick) m_on_pick(m_dialog.get_selected_path());
			return false;
		}
		if (m_answer) {
			if (!m_answer->cancel && m_on_pick) m_on_pick(std::filesystem::path(m_answer->path));
			return false;
		}
		return is_open;
	}

	const char* kind() const override { return "pick_file"; }
	std::string text() const override { return m_options.title; }

	json::value answer_schema() const override
	{
		return json::parse(R"({"type":"object","properties":{"path":{"type":"string","minLength":1},"cancel":{"type":"boolean"}},"minProperties":1,"additionalProperties":false})");
	}

	std::optional<std::string> accept(const json::value& answer) override
	{
		return read_path_answer(answer, m_answer);
	}

	void settle() override
	{
		if (m_answer && !m_answer->cancel && m_on_pick) m_on_pick(std::filesystem::path(m_answer->path));
	}

private:
	edt::pick_file_options m_options;
	std::function<void(const std::filesystem::path&)> m_on_pick;
	edt::file_dialog m_dialog;
	std::optional<path_answer> m_answer;
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

		bool is_open = true;
		if (begin_modal(label, MODAL_FLAGS)) {
			ImGui::TextUnformatted(m_text.c_str());
			ImGui::Spacing();
			// An answer from outside (dialog_service::answer) is a click on OK.
			if (ImGui::Button("OK", {120.f, 0.f}) || m_acknowledged) {
				is_open = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		} else {
			is_open = false;
		}
		return is_open;
	}

	const char* kind() const override { return "message"; }
	std::string text() const override { return m_text; }

	json::value answer_schema() const override
	{
		return json::parse(R"({"type":"object","properties":{},"additionalProperties":false})");
	}

	std::optional<std::string> accept(const json::value&) override
	{
		m_acknowledged = true;
		return std::nullopt;
	}

	void settle() override {}

private:
	edt::message_level m_level;
	std::string m_text;
	bool m_acknowledged = false;
};

} // namespace

edt::dialog_service::dialog_service() = default;

edt::dialog_service::~dialog_service()
{
	m_alive.reset();
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

std::vector<edt::dialog_service::dialog_info> edt::dialog_service::list() const
{
	std::vector<dialog_info> out;
	out.reserve(m_queue.size());
	for (const auto& r : m_queue) {
		dialog_info info;
		info.id = r->id;
		info.kind = r->kind();
		info.text = r->text();
		info.answer_schema = r->answer_schema();
		info.on_screen = r->drawn;
		out.push_back(std::move(info));
	}
	return out;
}

cmd::result edt::dialog_service::answer(dialog_id id, const json::value& answer)
{
	const auto found = std::find_if(m_queue.begin(), m_queue.end(), [id](const auto& r) { return r->id == id; });
	if (found == m_queue.end()) return cmd::result::failure(cmd::codes::not_found, std::format("no pending dialog {}", id));
	request& req = **found;
	if (req.answered) return cmd::result::failure(cmd::codes::failed, std::format("dialog {} was already answered", id));
	if (auto problem = cmd::json_schema::validate(req.answer_schema(), answer)) {
		return cmd::result::failure(cmd::codes::invalid_args, std::format("invalid answer for dialog {}: {}", id, *problem));
	}
	if (auto problem = req.accept(answer)) {
		return cmd::result::failure(cmd::codes::invalid_args, std::format("invalid answer for dialog {}: {}", id, *problem));
	}
	req.answered = true;

	const bool resolved_now = !req.drawn;
	if (resolved_now) {
		// Nothing of it is on screen, so there is no popup to close: act as the click would and drop it. The
		// callback may ask for more dialogs; the request stays where it is until then.
		req.settle();
		const auto done = std::find_if(m_queue.begin(), m_queue.end(), [&req](const auto& r) { return r.get() == &req; });
		if (done != m_queue.end()) m_queue.erase(done);
	}
	return cmd::result::success(json::object{ { "id", static_cast<std::int64_t>(id) }, { "resolved", resolved_now } });
}

bool edt::dialog_service::is_pending(dialog_id id) const
{
	return std::any_of(m_queue.begin(), m_queue.end(), [id](const auto& r) { return r->id == id; });
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
			[this, alive = std::weak_ptr<void>(m_alive)]() {
				return !alive.expired() && update();
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
	bool open = false;
	if (!answered) {
		front.drawn = true;
		open = front.draw();
	}
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
