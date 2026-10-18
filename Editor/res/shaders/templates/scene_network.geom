#version 420 core
layout (triangles) in;
layout (triangle_strip, max_vertices = 1) out;

layout (std140, binding = 0) uniform Matrices 
{
    mat4 projection;
    mat4 view;    
    float time;
};

void main() {
    //  p0 ---- p1
    //  |       |
    //  |       |
    //  p2 ---- p3

    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
    gl_Position = gl_in[1].gl_Position;
    EmitVertex();
    gl_Position = gl_in[2].gl_Position;
    EmitVertex();
    EndPrimitive();
}