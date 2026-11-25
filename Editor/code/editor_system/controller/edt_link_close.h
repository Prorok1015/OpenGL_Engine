#pragma once
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	// Closing a link closes everything opened from it (EPIC-82 5.4, 10), after ONE question for the whole chain; closing a host asks
	// nothing and is `document_manager::close_host`. No ImGui.
	struct chain_close_plan
	{
		// Deepest first, `id` last: the order they close in.
		std::vector<document_id> order;
		// The ones of `order` with unsaved changes.
		std::vector<document_id> dirty;
	};

	chain_close_plan plan_chain_close(document_manager& documents, document_id id);

	enum class chain_answer { save, discard, cancel };

	struct chain_close_ports
	{
		// The one question: the titles of the documents with unsaved changes. Called at most once per request.
		std::function<void(const std::vector<std::string>& dirty_titles, std::function<void(chain_answer)> answer)> ask;
		// Saves one document; an empty text is success, else why it was not saved. Unset: `document::save`.
		std::function<std::string(document_id)> save;
		// Closes one document, the descendants of the chain have been closed before it. Unset: `document_manager::close`.
		std::function<void(document_id)> close;
	};

	// Closes at once when nothing is unsaved. Otherwise asks once: Save writes every dirty document and closes the chain only when all were
	// saved (the first failure is returned through `failed`, nothing is closed); Discard closes; Cancel leaves everything open.
	// `failed` gets the text of a refused save. True when the chain was closed.
	bool request_close_link(document_manager& documents, document_id id, const chain_close_ports& ports, const std::function<void(const std::string&)>& failed = {});
}
