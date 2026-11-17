#include <snk/resource/res_tag_json.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_reference.h>

const res::tag res::tag::null{};

res::tag res::operator+ (const res::tag& l, const res::tag& r)
{
	if (l == r) {
		egLOG("resoure/tag", "Equal Tags when resource::operator+ (const resource::tag& l, const resource::tag& r)");
		return l;
	}

	const std::string path = std::vformat("{}{}{}", std::make_format_args(l.path(), r.path(), r.name()));
	return tag(l.protocol(), path);
}

void res::tag_invoke(json::value_from_tag, json::value& out, const res::tag& c)
{
	if (c.is_valid()) {
		out = json::value_from(c.view());
		return;
	}
	out = json::value{ nullptr };
}

res::tag res::tag_invoke(json::value_to_tag<res::tag>, const json::value& obj)
{
	if (obj.is_string())
		return res::tag{ obj.as_string() };
	// A {guid, path} reference (#758) reads as its path: a reader that has no resource_system cannot look up the guid.
	if (const auto reference = res::reference_from_json(obj))
		return reference->path;
	return res::tag::null;
}