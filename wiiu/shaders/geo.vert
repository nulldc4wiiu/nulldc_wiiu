#version 330
// PVR polygons. The release build uses only clip mode (u_screen.x = 0); the others are kept for testing.
// Clip mode: position is already in clip space (computed on the CPU from screen x/y and 1/w).
// Screen mode (u_screen.x > 0.5, as Flycast): position is the screen position with w = 1 and the depth, a_pos.w
// carries 1/w; colours and UVs are interpolated as value * 1/w and divided per pixel, so the GPU only clips flat
// triangles, as the PVR draws them (near-camera vertices with screen coordinates in the millions, MvC2's sky).
// Raw mode (u_screen.x 2, or 3 for half-range depth): a_pos is the TA's screen x, y and 1/w as decoded, and this
// does the screen and depth scaling (u_screen.yzw: 2 / width, 2 / height, depth scale), not the CPU
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec4 a_spc;
layout(location = 3) in vec2 a_uv;
uniform vec4 u_screen;
out vec4 v_col;
out vec4 v_spc;
// the same colours unblended, for polygons without Gouraud shading (the last vertex of each triangle wins)
flat out vec4 f_col;
flat out vec4 f_spc;
out vec2 v_uv;
out float v_w;    // clip mode: W, the eye distance; perspective-correct interpolation of it gives the per-pixel value
out float v_iw;   // screen mode: 1/W, interpolated linearly; the attributes above are pre-multiplied by it
void main()
{
    f_col = a_col;
    f_spc = a_spc;
    if (u_screen.x > 1.5)
    {
        float iw = a_pos.z > 1e-7 ? a_pos.z : 1e-7;   // NaN too, as the CPU conversion does (max() kept it)
        float d = min(iw * u_screen.w, 1.0);
        v_col = a_col * iw;
        v_spc = a_spc * iw;
        v_uv = a_uv * iw;
        v_iw = iw;
        v_w = 1.0;
        gl_Position = vec4(a_pos.x * u_screen.y - 1.0, 1.0 - a_pos.y * u_screen.z, u_screen.x > 2.5 ? d : 2.0 * d - 1.0, 1.0);
    }
    else if (u_screen.x > 0.5)
    {
        float iw = a_pos.w;
        v_col = a_col * iw;
        v_spc = a_spc * iw;
        v_uv = a_uv * iw;
        v_iw = iw;
        v_w = 1.0;
        gl_Position = vec4(a_pos.xyz, 1.0);
    }
    else
    {
        v_col = a_col;
        v_spc = a_spc;
        v_uv = a_uv;
        v_iw = 1.0;
        v_w = a_pos.w;
        gl_Position = a_pos;
    }
}
