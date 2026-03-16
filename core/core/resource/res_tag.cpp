#include "res_tag.h"
#include <engine_log.h>

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
	out = json::value_from(c.view());
}

res::tag res::tag_invoke(json::value_to_tag<res::tag>, const json::value& obj)
{
	return res::tag{ obj.as_string() };
}