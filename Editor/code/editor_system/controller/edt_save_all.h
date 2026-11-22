#pragma once
#include <snk/editor/edt_document.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class document_manager;

	// The chord of File > Save All. The one place to change it.
	inline constexpr std::string_view k_save_all_shortcut = "Ctrl+Shift+S";

	// Save All (#1183, EPIC-59): the documents with unsaved changes, what one write of all of them did, and the questions
	// that follow it. No ImGui and no dialog in here: the File menu and the agent's action run it with their own answers.

	// One open document with unsaved changes.
	struct unsaved_item
	{
		document_id id = 0;
		std::string kind;
		std::string title;
	};

	// The documents with unsaved changes: the scene, then the prefab, then the data documents in the order they were opened.
	std::vector<unsaved_item> list_unsaved(document_manager& documents);
	// How many there are, without building the list (the menu bar asks at every frame).
	std::size_t count_unsaved(document_manager& documents);

	// What a document that could not be written needs from the person.
	enum class save_all_question : std::uint8_t
	{
		disk_changed,	// the file changed on disk: Reload / Overwrite / Cancel
		version,		// a migrated file is written in its new version only after a yes
		base_layer,		// a variant prefab whose edits no layer over its base can say: Save without base / Cancel
	};

	struct save_all_note
	{
		unsaved_item item;
		std::string text;
	};

	struct save_all_ask
	{
		unsaved_item item;
		save_all_question question = save_all_question::disk_changed;
	};

	struct save_all_report
	{
		std::vector<unsaved_item> saved;
		std::vector<save_all_note> failed;	// text: why it was not written
		std::vector<save_all_ask> asked;	// not written, waits for an answer
		std::vector<save_all_note> skipped;	// text: why it is not part of Save All
		std::vector<unsaved_item> settled;	// an answer of the person ended it (reloaded or overwritten)

		bool empty() const { return saved.empty() && failed.empty() && asked.empty() && skipped.empty() && settled.empty(); }
		// Something the person has to know about: a failure, a document still unsaved or one that was left out.
		bool needs_attention() const { return !failed.empty() || !asked.empty() || !skipped.empty(); }
	};

	// What Save All needs to write the level, which the document does not do by itself.
	struct save_all_scene_ports
	{
		// Why the level may not be written now (a level is loading, a migration runs); null or nullopt: it may.
		std::function<std::optional<std::string>()> write_blocker;
		// Res-relative path of the saved level, empty for an untitled one.
		std::function<std::string()> level_path;
		// Overwrites the saved level, true if written.
		std::function<bool()> quick_save;
	};

	// One row of the unsaved list: a document and, after a Save All, what is wrong with it (empty: nothing to add).
	struct unsaved_row
	{
		document_id id = 0;
		std::string title;
		std::string note;
	};

	// The rows of one kind of document under its heading.
	struct unsaved_group
	{
		std::string kind;
		std::string heading;
		std::vector<unsaved_row> rows;
	};

	// The one list the menu bar's popup, its tooltip and the message after a Save All show: grouped by kind (level, prefab, data,
	// the rest), in the order the documents came in.
	std::vector<unsaved_group> unsaved_groups(const std::vector<unsaved_item>& items);
	// What is still unsaved after a Save All (failed, waiting for an answer, left out), each row with its reason.
	std::vector<unsaved_group> unsaved_groups(const save_all_report& report);
	// `heading`, then every group's heading with its rows under it.
	std::string unsaved_groups_text(const std::vector<unsaved_group>& groups, std::string_view heading);
	// The dirty documents as text (tooltip).
	std::string unsaved_text(const std::vector<unsaved_item>& items);

	// What a document that refused to be saved waits for (nullopt: nothing, the refusal is a failure). Looks at data and prefab documents.
	std::optional<save_all_question> save_all_question_of(const document& open);

	// Writes every document that can be written and goes on after one that cannot: a document that waits for a question
	// (a file that changed on disk, a migrated file, a variant prefab) is collected in `asked`, an untitled level, a level
	// of a running Play are `skipped`, any other refusal is `failed`. Asks nothing.
	save_all_report save_all_write(document_manager& documents, const save_all_scene_ports& scene);

	// The question of one `asked` item; `resolved(true)` once the document was reloaded or overwritten, `resolved(false)` after Cancel
	// or a failure.
	using save_all_asker = std::function<void(const save_all_ask& ask, std::function<void(bool resolved)> resolved)>;

	// Asks about the `asked` items of `report` one after the other. What the person settled moves to `settled`, what they
	// cancelled stays in `asked` (and unsaved); `finished` gets the report after the last answer. Without `ask`, `finished`
	// runs at once with the report as it is.
	void save_all_resolve(save_all_report report, save_all_asker ask, std::function<void(save_all_report)> finished);

	// The report as the action's result: {saved, failed, needs_input, skipped}, each a list of {id, kind, title} (failed also
	// `error`, needs_input `question`, skipped `reason`).
	boost::json::value save_all_json(const save_all_report& report);
	// One message for the person: what was saved, what was not and why. Empty when there is nothing to tell.
	std::string save_all_summary(const save_all_report& report);
	// The question of an item in words ("changed on disk"), for the summary and the agent's answer.
	std::string_view save_all_question_text(save_all_question question);
}
