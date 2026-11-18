#pragma once
#include "edt_asset_commands.h"
#include "edt_asset_database.h"
#include "edt_asset_ops.h"
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the asset browser's Rename, Move and Delete windows hold between frames, without ImGui (EPIC-57 section 6.4, #775): the
// request, the plan as of the last change of the input (the files that move, who refers to them, what is rewritten, why it is
// refused), the second question of a forced delete, the report of the operation and its undo. The panel only draws it.
namespace edt
{
	// Why an operation was refused, as a person reads it: a sentence per refusal_code, then the refusal's own reason and the files
	// it names (the open documents, the unrewritable files, the places that refer to it). One text, lines apart by '\n'.
	std::string refusal_message(const refusal& why);

	struct asset_operation_ports
	{
		// Reads the disk, changes nothing (asset_ops::plan).
		std::function<operation_plan(const asset_request&)> plan;
		// The same path as the commands (run_asset_operation), asked as a person's call.
		std::function<asset_run(const asset_request&, bool force)> run;
		// Takes the operation back (asset_component::undo_operation); refused when it is not the newest or a file changed since.
		std::function<asset_fix_result(const std::string& operation_id)> undo;
	};

	class asset_operation_form
	{
	public:
		enum class stage : std::uint8_t
		{
			closed,
			editing,
			// A delete that leaves references broken: the second question, apart from the first.
			confirming_force,
			// The operation was done: the report, with its undo.
			reported,
		};

		explicit asset_operation_form(asset_operation_ports ports);

		// `asset` is a res:// tag. Rename starts with the file's name, move with its folder; both are planned at once.
		void open(asset_operation what, const res::tag& asset);
		void close();
		stage current() const { return m_stage; }
		asset_operation operation() const { return m_operation; }
		const res::tag& asset() const { return m_asset; }

		// rename: the new file name; move: the folder below res:// ("" is the root); delete: not used. Plans again (the disk is read).
		const std::string& input() const { return m_input; }
		void set_input(std::string input);
		// Plans again, for a disk that changed meanwhile.
		void replan();

		const plan_item* item() const { return m_item ? &*m_item : nullptr; }
		const refusal* refused() const { return m_refused ? &*m_refused : nullptr; }
		// refusal_message of refused(); empty when planned.
		std::string refusal_text() const;
		// What the operation would do, one line each: the target, the files that move, the references rewritten per file and
		// pointer, the ones that cannot be, the ones that will break, the warnings; for a delete who refers to the asset.
		std::vector<std::string> preview_lines() const;

		// The default action: planned, nothing refused, and (a delete) nothing refers to the asset and its users are all known.
		bool can_submit() const;
		// A delete with referrers (or with a scan that did not read every file): only `force` goes on, after its own question.
		bool needs_force() const;
		bool can_ask_force() const;
		void ask_force();
		void back();
		// What the second question says: what would break, the places capped with a count.
		std::string force_text() const;

		// Runs the operation without force (default action) or with it (only after ask_force()); false: not done, error() says why.
		bool submit();
		bool submit_force();
		// What the last run said when it did not go through, as a person reads it.
		const std::string& error() const { return m_error; }

		const asset_run& result() const { return m_run; }
		// The report of the operation done: what moved, what was rewritten (with pointers), what could not be, warnings, what is broken.
		std::vector<std::string> report_lines() const;
		std::string report_title() const;
		bool can_undo() const { return m_stage == stage::reported && !m_undone && !m_run.report.operation_id.empty(); }
		bool undo();
		bool undone() const { return m_undone; }
		const std::string& undo_error() const { return m_undo_error; }

	private:
		asset_request request() const;
		bool run(bool force);

		asset_operation_ports m_ports;
		stage m_stage = stage::closed;
		asset_operation m_operation = asset_operation::rename;
		res::tag m_asset;
		std::string m_input;
		std::optional<plan_item> m_item;
		std::optional<refusal> m_refused;
		std::string m_error;
		asset_run m_run;
		bool m_undone = false;
		std::string m_undo_error;
	};
}
