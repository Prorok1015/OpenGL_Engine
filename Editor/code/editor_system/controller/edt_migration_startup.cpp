#include "edt_migration_startup.h"
#include <format>

namespace {
	// A journal's folder name is whatever is on the disk: shown without control characters, cut short.
	std::string shown_id(std::string_view id)
	{
		std::string out;
		out.reserve(id.size() < 128 ? id.size() : 128);
		for (const char c : id) {
			if (out.size() >= 128) {
				out += "...";
				break;
			}
			const auto byte = static_cast<unsigned char>(c);
			out.push_back(byte < 0x20 || byte == 0x7f ? '?' : c);
		}
		return out;
	}
}

edt::journal_offer edt::decide_journal_offer(const std::vector<std::string>& unfinished, bool headless)
{
	journal_offer offer;
	if (unfinished.empty()) return offer;
	offer.id = unfinished.front();
	const std::string id = shown_id(offer.id);
	const std::string others = unfinished.size() > 1 ? std::format(" ({} more after it)", unfinished.size() - 1) : std::string{};
	if (headless) {
		offer.what = journal_offer::kind::warn;
		offer.text = std::format("migration {} did not finish{}: the project may be partly migrated; roll it back with "
			"editor --headless --project <dir> --migrate-rollback {}", id, others, id);
		return offer;
	}
	offer.what = journal_offer::kind::ask;
	offer.title = std::format("Migration {} did not finish", id);
	offer.text = std::format("The migration {} of this project did not finish{}: the project may be partly migrated.\n"
		"Roll it back now? Every file it wrote is put back from .snk/migrations/{}/backup.\n"
		"Later: Project > Migrate Project... offers it again, or run editor --migrate-rollback {}.", id, others, id, id);
	return offer;
}
