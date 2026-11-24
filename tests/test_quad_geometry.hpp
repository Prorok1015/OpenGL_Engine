#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace test_util {

// A geometry_desc vertex array is bytes, one JSON number each.
inline std::string bytes_json(const std::vector<float>& floats)
{
	std::string out = "[";
	for (const float value : floats) {
		unsigned char raw[sizeof(float)];
		std::memcpy(raw, &value, sizeof(float));
		for (const unsigned char byte : raw) {
			out += std::to_string(static_cast<int>(byte)) + ",";
		}
	}
	if (out.size() > 1) {
		out.pop_back();
	}
	return out + "]";
}

// A square of the given half size in the XY plane, two counter-clockwise triangles: the front faces +Z.
inline std::string quad_geometry_json(float half = 0.5f)
{
	const std::vector<float> corners{ -half, -half, 0.f, half, -half, 0.f, half, half, 0.f, -half, half, 0.f };
	return R"({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[0,1,2,0,2,3],"vertices":)" + bytes_json(corners) + "}";
}

}
