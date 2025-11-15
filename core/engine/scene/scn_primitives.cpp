#include "scn_primitives.h"
#include <rnd_render_system.h>

#include <numeric>

scn::model_web scn::generate_web(glm::ivec2 size)
{
    std::vector<vertex> vex;
    std::vector<unsigned int> inc;

    const int half_x = size.x / 2;
    for (int i = -half_x; i <= half_x; ++i) {
        vertex line[2];
        line[0].position = { 1.f / half_x * i, 0.f, -1.f };
        line[1].position = { 1.f / half_x * i, 0.f,  1.f };

        vex.push_back(line[0]);
        vex.push_back(line[1]);
        inc.push_back((unsigned)vex.size() - 2);
        inc.push_back((unsigned)vex.size() - 1);
    }

    const int half_y = size.y / 2;
    for (int i = -half_y; i <= half_y; ++i) {
        vertex line[2];
        line[0].position = { 1.f, 0.f, 1.f / half_y * i };
        line[1].position = { -1.f, 0.f, 1.f / half_y * i };

        vex.push_back(line[0]);
        vex.push_back(line[1]);
        inc.push_back((unsigned)vex.size() - 2);
        inc.push_back((unsigned)vex.size() - 1);
    }

    return { vex, inc };
}

scn::model_cube scn::generate_cube()
{
    std::vector<vertex> vex = {
    {.position = {-1.f, -1.f, -1.f}, .uv = {0, 0}}, // 0
    {.position = {-1.f, 1.f, -1.f}, .uv = {0, 1}}, // 1
    {.position = {1.f, 1.f, -1.f}, .uv = {1, 1}}, // 2
    {.position = {1.f, -1.f, -1.f}, .uv = {1, 0}}, // 3

    {.position = {-1.f, -1.f, 1.f}, .uv = {0, 0}}, // 4
    {.position = {1.f, -1.f, 1.f}, .uv = {1, 0}}, // 5
    {.position = {1.f, 1.f, 1.f}, .uv = {1, 1}}, // 6
    {.position = {-1.f, 1.f, 1.f}, .uv = {0, 1}}  // 7
    };

    std::vector<unsigned int> inc = {
        0, 1, 3,
        3, 1, 2,

        4, 5, 7,
        7, 5, 6,

        0, 4, 1,
        1, 4, 7,

        2, 6, 3,
        3, 6, 5,

        1, 7, 2,
        2, 7, 6,

        0, 3, 4,
        4, 3, 5
    };
    //std::vector<res::Vertex> vex
    //{
    //    {.position = {-1.f, -1.f, -1.f}, .uv = {0,0}}, // 0
    //    {.position = { -1.f, 1.f, -1.f}, .uv = {0,1}}, // 1
    //    {.position = { 1.f,  1.f, -1.f}, .uv = {1,1}}, // 2
    //    {.position = { 1.f, -1.f, -1.f}, .uv = {1,0}}, // 3

    //    {.position = {-1.f, -1.f, 1.f}, .uv = {1,0}},  // 4
    //    {.position = { 1.f, -1.f, 1.f}, .uv = {0,0}},  // 5
    //    {.position = { 1.f,  1.f, 1.f}, .uv = {0,1}},  // 6
    //    {.position = {-1.f,  1.f, 1.f}, .uv = {1,1}},  // 7

    //    {.position = {-1.f, -1.f,-1.f}, .uv = {0,0}},  // 8
    //    {.position = {-1.f, -1.f, 1.f}, .uv = {1,0}},  // 9
    //    {.position = {-1.f,  1.f, 1.f}, .uv = {1,1}},  // 10
    //    {.position = {-1.f,  1.f,-1.f}, .uv = {0,1}},  // 11

    //    {.position = { 1.f, -1.f, -1.f}, .uv = {1,0}}, // 12
    //    {.position = { 1.f,  1.f, -1.f}, .uv = {0,0}}, // 13
    //    {.position = { 1.f,  1.f,  1.f}, .uv = {0,1}}, // 14
    //    {.position = { 1.f,  -1.f, 1.f}, .uv = {1,1}}, // 15

    //    {.position = {-1.f, -1.f, -1.f}, .uv = {0,0}}, // 16
    //    {.position = { 1.f, -1.f, -1.f}, .uv = {1,0}}, // 17
    //    {.position = { 1.f, -1.f,  1.f}, .uv = {1,1}}, // 18
    //    {.position = {-1.f, -1.f,  1.f}, .uv = {0,1}}, // 19

    //    {.position = {-1.f,  1.f, -1.f}, .uv = {0,0}}, // 20
    //    {.position = {-1.f,  1.f,  1.f}, .uv = {1,0}}, // 21
    //    {.position = { 1.f,  1.f,  1.f}, .uv = {1,1}}, // 22
    //    {.position = { 1.f,  1.f, -1.f}, .uv = {0,1}}, // 23
    //};
    //std::vector<unsigned int> inc
    //{
    //    /*
    //        0 --- 1
    //        |    /|
    //        |  /  |
    //        |/    |
    //        3 --- 2
    //    */
    //    0,1,3,
    //    3,1,2,

    //    4,5,6,
    //    6,5,7,

    //    8,9,11,
    //    11,9,10,

    //    12,13,15,
    //    15,13,14,

    //    16,17,19,
    //    19,17,18,

    //    20,21,23,
    //    23,21,22,
    //};

    for (int i = 0; i < inc.size(); i += 3) {
        auto& p1 = vex[inc[i]];
        auto& p2 = vex[inc[i + 1]];
        auto& p3 = vex[inc[i + 2]];

        p1.normal = glm::normalize(glm::cross(p3.position - p2.position, p1.position - p2.position));
        p2.normal = p1.normal;
        p3.normal = p1.normal;
    }

    return { vex, inc };
}

std::vector<scn::vertex> generate_sphere_data(float radius, float sectorCount, float stackCount)
{
    std::vector<scn::vertex> result;

    constexpr float PI = (float)std::numbers::pi;
    float x, y, z, xy;                              // vertex position
    float nx, ny, nz, lengthInv = 1.0f / radius;    // vertex normal
    float s, t;                                     // vertex texCoord

    float sectorStep = 2 * PI / sectorCount;
    float stackStep = PI / stackCount;

    for (int i = 0; i <= stackCount; ++i)
    {
        const float stackAngle = PI / 2 - i * stackStep;        // starting from pi/2 to -pi/2
        xy = radius * std::cos(stackAngle);             // r * cos(u)
        z = radius * std::sin(stackAngle);              // r * sin(u)

        // add (sectorCount+1) vertices per stack
        // first and last vertices have same position and normal, but different tex coords
        for (int j = 0; j <= sectorCount; ++j)
        {
            const float sectorAngle = j * sectorStep;           // starting from 0 to 2pi

            scn::vertex vert;

            // vertex position (x, y, z)
            x = xy * std::cos(sectorAngle);             // r * cos(u) * cos(v)
            y = xy * std::sin(sectorAngle);             // r * cos(u) * sin(v)
            vert.position = glm::vec3(x, y, z);

            // normalized vertex normal (nx, ny, nz)
            nx = x * lengthInv;
            ny = y * lengthInv;
            nz = z * lengthInv;
            vert.normal = glm::vec3(nx, ny, nz);

            // vertex tex coord (s, t) range between [0, 1]
            s = (float)j / sectorCount;
            t = (float)i / stackCount;
            vert.uv = glm::vec2(s, t);
            result.push_back(vert);
        }
    }

    return result;
}

scn::model_sphere scn::generate_sphere()
{
    float radius = 1.0f;
    float sectorCount = 72.f;
    float stackCount = 24.f;

    auto vex = generate_sphere_data(radius, sectorCount, stackCount);

    // generate CCW index list of sphere triangles
    // k1--k1+1
    // |  / |
    // | /  |
    // k2--k2+1
    std::vector<unsigned int> inc;
    std::vector<int> lineIndices;

    for (int i = 0; i < stackCount; ++i)
    {
        int k1 = i * (int)(sectorCount + 1);     // beginning of current stack
        int k2 = k1 + (int)sectorCount + 1;      // beginning of next stack

        for (int j = 0; j < (int)sectorCount; ++j, ++k1, ++k2)
        {
            // 2 triangles per sector excluding first and last stacks
            // k1 => k2 => k1+1
            if (i != 0)
            {
                inc.push_back(k1);
                inc.push_back(k2);
                inc.push_back(k1 + 1);
            }

            // k1+1 => k2 => k2+1
            if (i != (stackCount - 1))
            {
                inc.push_back(k1 + 1);
                inc.push_back(k2);
                inc.push_back(k2 + 1);
            }

            // store indices for lines
            // vertical lines for all stacks, k1 => k2
            lineIndices.push_back(k1);
            lineIndices.push_back(k2);
            if (i != 0)  // horizontal lines except 1st stack, k1 => k+1
            {
                lineIndices.push_back(k1);
                lineIndices.push_back(k1 + 1);
            }
        }
    }

    for (auto& v : vex) {
        v.uv = v.uv / glm::vec2(16, -16);
    }

    return { vex, inc };
}
