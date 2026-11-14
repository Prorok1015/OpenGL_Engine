#include "edt_asset_picker.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace
{
	std::string lowered(std::string_view text)
	{
		std::string out{ text };
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	edt::picker::verdict rejected(std::string reason)
	{
		return { edt::picker::verdict::result::rejected, std::move(reason) };
	}
}

void edt::picker::database_catalog::for_each(const std::function<void(const asset_record&)>& visit) const
{
	for (const auto& [key, record] : m_database.records()) {
		visit(record);
	}
}

edt::picker::filter edt::picker::filter_of(const desc::field_schema& field)
{
	filter out;
	out.kind = field.kind;
	if (field.kind == desc::field_kind::desc_ref) {
		out.ref_type = field.ref_type;
	}
	out.extensions = field.filter;
	return out;
}

bool edt::picker::extension_matches(std::string_view path, std::string_view extensions)
{
	if (extensions.empty()) {
		return true;
	}
	const std::string lower = lowered(path);
	std::size_t start = 0;
	while (start <= extensions.size()) {
		std::size_t end = extensions.find_first_of(",;| ", start);
		if (end == std::string_view::npos) {
			end = extensions.size();
		}
		std::string_view token = extensions.substr(start, end - start);
		while (!token.empty() && (token.front() == '*' || token.front() == '.')) {
			token.remove_prefix(1);
		}
		if (!token.empty()) {
			const std::string suffix = "." + lowered(token);
			if (lower.ends_with(suffix)) {
				return true;
			}
		}
		start = end + 1;
	}
	return false;
}

std::string edt::picker::layer_label(std::size_t layer, std::size_t layer_count)
{
	if (layer_count <= 1 || layer + 1 >= layer_count) {
		return "project";
	}
	return layer == 0 ? "engine" : "package";
}

edt::picker::verdict edt::picker::judge(const filter& wanted, const asset_record& record)
{
	if (!extension_matches(record.path.relative(), wanted.extensions)) {
		return rejected(std::format("does not match the filter '{}'", wanted.extensions));
	}
	if (wanted.kind != desc::field_kind::desc_ref) {
		return {};
	}
	if (record.type != "desc") {
		return rejected("is not a desc file");
	}
	if (wanted.ref_type.empty()) {
		return {};
	}
	if (record.desc_type) {
		if (*record.desc_type == wanted.ref_type) {
			return {};
		}
		return rejected(std::format("is a desc of type '{}', expected '{}'", *record.desc_type, wanted.ref_type));
	}
	if (record.content_hash.empty()) {
		return { verdict::result::unknown, "its type is not read yet" };
	}
	return rejected(std::format("is a desc with no type, expected '{}'", wanted.ref_type));
}

edt::picker::candidates_page edt::picker::candidates(const catalog& source, const filter& wanted, std::string_view query, std::size_t limit)
{
	const std::string needle = lowered(query);
	const std::size_t layers = source.layer_count();
	candidates_page page;
	// The sort key is made once per file, not at every comparison.
	struct keyed
	{
		std::string key;
		const asset_record* record = nullptr;
	};
	std::vector<keyed> fitting;
	source.for_each([&](const asset_record& record) {
		const verdict answer = judge(wanted, record);
		if (answer.outcome == verdict::result::rejected) {
			return;
		}
		std::string key = lowered(record.path.relative());
		if (!needle.empty() && key.find(needle) == std::string::npos) {
			return;
		}
		if (answer.outcome == verdict::result::unknown) {
			++page.unread;
		} else {
			fitting.push_back({ std::move(key), &record });
		}
	});
	std::ranges::sort(fitting, [](const keyed& a, const keyed& b) {
		if (a.record->layer != b.record->layer) {
			return a.record->layer > b.record->layer;
		}
		return a.key < b.key;
	});
	page.total = fitting.size();
	const std::size_t shown = std::min(limit, fitting.size());
	page.items.reserve(shown);
	for (std::size_t i = 0; i < shown; ++i) {
		page.items.push_back({ fitting[i].record, layer_label(fitting[i].record->layer, layers) });
	}
	return page;
}

const edt::picker::candidates_page& edt::picker::cached_candidates(page_cache& cache, const catalog& source, const filter& wanted, std::string_view query,
	std::size_t limit)
{
	const std::uint64_t revision = source.revision();
	if (!cache.valid || cache.revision != revision || cache.limit != limit || !(cache.wanted == wanted) || cache.query != query) {
		cache.page = candidates(source, wanted, query, limit);
		cache.wanted = wanted;
		cache.query = std::string(query);
		cache.limit = limit;
		cache.revision = revision;
		cache.valid = true;
	}
	return cache.page;
}

boost::json::value edt::picker::reference_value(const asset_record& record)
{
	return boost::json::string(record.path.string());
}

edt::picker::reference_state edt::picker::judge_text(const filter& wanted, std::string_view text, const catalog& source)
{
	if (text.empty() || !source.ready()) {
		return {};
	}
	const res::tag tag{ text };
	if (tag.protocol() != res::tag::default_protocol()) {
		return {};
	}
	const asset_record* record = source.find(tag);
	if (!record) {
		if (!source.settled()) {
			return { reference_state::level::note, std::format("{} is not in the asset list yet", tag.string()) };
		}
		return { reference_state::level::error, std::format("file not found: {}", tag.string()) };
	}
	const verdict answer = judge(wanted, *record);
	switch (answer.outcome) {
	case verdict::result::fits: return {};
	case verdict::result::unknown: return { reference_state::level::note, answer.reason };
	case verdict::result::rejected: break;
	}
	return { reference_state::level::error, std::format("'{}' {}", tag.string(), answer.reason) };
}

edt::picker::verdict edt::picker::judge_drop(const filter& wanted, std::string_view path, const catalog* source)
{
	if (!source || !source->ready()) {
		if (!extension_matches(path, wanted.extensions)) {
			return rejected(std::format("does not match the filter '{}'", wanted.extensions));
		}
		return {};
	}
	const asset_record* record = source->find(res::tag::make(path));
	if (!record) {
		return { verdict::result::unknown, "is not in the asset list yet" };
	}
	return judge(wanted, *record);
}
