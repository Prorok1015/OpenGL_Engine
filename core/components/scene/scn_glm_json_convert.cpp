#include "scn_glm_json_convert.h"

void glm::tag_invoke(json::value_from_tag, json::value& jv, const glm::vec2& v) {
    jv = json::array{ v.x, v.y };
}

void glm::tag_invoke(json::value_from_tag, json::value& jv, const glm::vec3& v) {
    jv = json::array{ v.x, v.y, v.z };
}

void glm::tag_invoke(json::value_from_tag, json::value& jv, const glm::vec4& v) {
    jv = json::array{ v.x, v.y, v.z, v.w };
}

void glm::tag_invoke(json::value_from_tag, json::value& jv, const glm::mat4& m) {
    jv = json::array{
        json::value_from( m[0] ),
		json::value_from( m[1] ),
		json::value_from( m[2] ),
		json::value_from( m[3] ),
    };
}

void glm::tag_invoke(json::value_from_tag, json::value& jv, const glm::quat& m)
{
	jv = json::array{ m.w, m.x, m.y, m.z };
}

glm::vec2 glm::tag_invoke(json::value_to_tag<glm::vec2>, const json::value& jv)
{
	if (jv.is_array() && jv.as_array().size() == 2) {
		return glm::vec2{
			json::value_to<float>(jv.at(0)),
			json::value_to<float>(jv.at(1))
		};
	}
    return {};
}
glm::vec3 glm::tag_invoke(json::value_to_tag<glm::vec3>, const json::value& jv)
{
	if (jv.is_array() && jv.as_array().size() == 3) {
		return glm::vec3{
			json::value_to<float>(jv.at(0)),
			json::value_to<float>(jv.at(1)),
			json::value_to<float>(jv.at(2))
		};
	}
	return {};
}
glm::vec4 glm::tag_invoke(json::value_to_tag<glm::vec4>, const json::value& jv)
{
	if (jv.is_array() && jv.as_array().size() == 4) {
		return glm::vec4{
			json::value_to<float>(jv.at(0)),
			json::value_to<float>(jv.at(1)),
			json::value_to<float>(jv.at(2)),
			json::value_to<float>(jv.at(3))
		};
	}
	return {};
}
glm::mat4 glm::tag_invoke(json::value_to_tag<glm::mat4>, const json::value& jv)
{
	if (jv.is_array() && jv.as_array().size() == 4) {
		return glm::mat4{
			json::value_to<glm::vec4>(jv.at(0)),
			json::value_to<glm::vec4>(jv.at(1)),
			json::value_to<glm::vec4>(jv.at(2)),
			json::value_to<glm::vec4>(jv.at(3))
		};
	}
	return {};
}
glm::quat glm::tag_invoke(json::value_to_tag<glm::quat>, const json::value& jv)
{
	if (jv.is_array() && jv.as_array().size() == 4) {
		return glm::quat{
			json::value_to<float>(jv.at(0)),
			json::value_to<float>(jv.at(1)),
			json::value_to<float>(jv.at(2)),
			json::value_to<float>(jv.at(3))
		};
	}
	return {};
}