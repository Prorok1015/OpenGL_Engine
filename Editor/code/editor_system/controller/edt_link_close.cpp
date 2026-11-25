#include "edt_link_close.h"
#include <format>
#include <memory>

namespace edt
{
	chain_close_plan plan_chain_close(document_manager& documents, document_id id)
	{
		chain_close_plan plan;
		if (!documents.find(id)) return plan;
		plan.order = documents.descendants_of(id);
		plan.order.push_back(id);
		for (const document_id member : plan.order) {
			if (const document* open = documents.find(member); open && open->is_dirty()) plan.dirty.push_back(member);
		}
		return plan;
	}

	namespace
	{
		void close_all(document_manager& documents, const chain_close_plan& plan, const chain_close_ports& ports)
		{
			for (const document_id member : plan.order) {
				if (!documents.find(member)) continue;
				if (ports.close) ports.close(member);
				else documents.close(member);
			}
		}

		std::string save_one(document_manager& documents, document_id id, const chain_close_ports& ports)
		{
			if (ports.save) return ports.save(id);
			document* open = documents.find(id);
			if (!open) return {};
			const save_result saved = open->save();
			return saved.saved ? std::string{} : (saved.error.empty() ? std::string("not saved") : saved.error);
		}
	}

	bool request_close_link(document_manager& documents, document_id id, const chain_close_ports& ports, const std::function<void(const std::string&)>& failed)
	{
		const chain_close_plan plan = plan_chain_close(documents, id);
		if (plan.order.empty()) return false;
		if (plan.dirty.empty() || !ports.ask) {
			if (!plan.dirty.empty()) return false;
			close_all(documents, plan, ports);
			return true;
		}
		std::vector<std::string> titles;
		for (const document_id member : plan.dirty) {
			if (const document* open = documents.find(member)) titles.push_back(open->title());
		}
		auto closed = std::make_shared<bool>(false);
		ports.ask(titles, [&documents, plan, ports, failed, closed](chain_answer answer) {
			if (answer == chain_answer::cancel) return;
			if (answer == chain_answer::save) {
				for (const document_id member : plan.dirty) {
					const document* open = documents.find(member);
					if (!open || !open->is_dirty()) continue;
					if (const std::string why = save_one(documents, member, ports); !why.empty()) {
						if (failed) failed(std::format("'{}' was not saved and stays open: {}", open->title(), why));
						return;
					}
				}
			}
			close_all(documents, plan, ports);
			*closed = true;
		});
		return *closed;
	}
}
