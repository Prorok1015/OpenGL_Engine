#include "edt_link_close.h"
#include <algorithm>
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

	namespace
	{
		struct pending_chain
		{
			document_manager& documents;
			document_id id;
			chain_close_ports ports;
			std::function<void(const std::string&)> failed;
			bool closed = false;
		};

		std::vector<std::string> titles_of(document_manager& documents, const std::vector<document_id>& dirty)
		{
			std::vector<std::string> titles;
			for (const document_id member : dirty) {
				if (const document* open = documents.find(member)) titles.push_back(open->title());
			}
			return titles;
		}

		void ask_about(const std::shared_ptr<pending_chain>& chain, const std::vector<document_id>& asked)
		{
			chain->ports.ask(titles_of(chain->documents, asked), [chain, asked](chain_answer answer) {
				if (answer == chain_answer::cancel) return;
				// The plan of the moment the answer comes: what was edited while the question was open counts.
				const chain_close_plan now = plan_chain_close(chain->documents, chain->id);
				if (now.order.empty()) return;
				if (answer == chain_answer::save) {
					for (const document_id member : now.dirty) {
						const document* open = chain->documents.find(member);
						if (!open || !open->is_dirty()) continue;
						if (const std::string why = save_one(chain->documents, member, chain->ports); !why.empty()) {
							if (chain->failed) chain->failed(std::format("'{}' was not saved and stays open: {}", open->title(), why));
							return;
						}
					}
				} else {
					const bool grew = std::ranges::any_of(now.dirty, [&asked](document_id member) { return std::ranges::find(asked, member) == asked.end(); });
					if (grew) {
						ask_about(chain, now.dirty);
						return;
					}
				}
				close_all(chain->documents, plan_chain_close(chain->documents, chain->id), chain->ports);
				chain->closed = true;
			});
		}
	}

	bool request_close_link(document_manager& documents, document_id id, const chain_close_ports& ports, const std::function<void(const std::string&)>& failed)
	{
		const chain_close_plan plan = plan_chain_close(documents, id);
		if (plan.order.empty()) return false;
		if (plan.dirty.empty()) {
			close_all(documents, plan, ports);
			return true;
		}
		if (!ports.ask) return false;
		auto chain = std::make_shared<pending_chain>(pending_chain{ documents, id, ports, failed });
		ask_about(chain, plan.dirty);
		return chain->closed;
	}
}
