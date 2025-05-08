#pragma once
#include "common.h"
#include <boost/json.hpp>

namespace json = boost::json;

namespace glm {
    void tag_invoke(json::value_from_tag, json::value& jv, const glm::vec2& v);
    void tag_invoke(json::value_from_tag, json::value& jv, const glm::vec3& v);
    void tag_invoke(json::value_from_tag, json::value& jv, const glm::vec4& v);
    void tag_invoke(json::value_from_tag, json::value& jv, const glm::mat4& m);

	glm::vec2 tag_invoke(json::value_to_tag<glm::vec2>, const json::value& jv);
	glm::vec3 tag_invoke(json::value_to_tag<glm::vec3>, const json::value& jv);
	glm::vec4 tag_invoke(json::value_to_tag<glm::vec4>, const json::value& jv);
	glm::mat4 tag_invoke(json::value_to_tag<glm::mat4>, const json::value& jv);
}
