#pragma once
#include "common.h"
#include <boost/json.hpp>
#include "snk_api.h"

namespace json = boost::json;

namespace glm {
    SNK_API void tag_invoke(json::value_from_tag, json::value& jv, const glm::vec2& v);
    SNK_API void tag_invoke(json::value_from_tag, json::value& jv, const glm::vec3& v);
    SNK_API void tag_invoke(json::value_from_tag, json::value& jv, const glm::vec4& v);
    SNK_API void tag_invoke(json::value_from_tag, json::value& jv, const glm::mat4& m);
    SNK_API void tag_invoke(json::value_from_tag, json::value& jv, const glm::quat& m);

	SNK_API glm::vec2 tag_invoke(json::value_to_tag<glm::vec2>, const json::value& jv);
	SNK_API glm::vec3 tag_invoke(json::value_to_tag<glm::vec3>, const json::value& jv);
	SNK_API glm::vec4 tag_invoke(json::value_to_tag<glm::vec4>, const json::value& jv);
	SNK_API glm::mat4 tag_invoke(json::value_to_tag<glm::mat4>, const json::value& jv);
	SNK_API glm::quat tag_invoke(json::value_to_tag<glm::quat>, const json::value& jv);
}
