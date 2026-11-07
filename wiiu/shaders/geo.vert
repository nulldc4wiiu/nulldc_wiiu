#version 330
// PVR polygons: position is already in clip space (computed on the CPU from screen x/y and 1/w)
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec4 a_spc;
layout(location = 3) in vec2 a_uv;
out vec4 v_col;
out vec4 v_spc;
// the same colours unblended, for polygons without Gouraud shading (the last vertex of each triangle wins)
flat out vec4 f_col;
flat out vec4 f_spc;
out vec2 v_uv;
out float v_w;   // W, the eye distance; perspective-correct interpolation of it gives the per-pixel value
void main()
{
    v_col = a_col;
    v_spc = a_spc;
    f_col = a_col;
    f_spc = a_spc;
    v_uv = a_uv;
    v_w = a_pos.w;
    gl_Position = a_pos;
}
