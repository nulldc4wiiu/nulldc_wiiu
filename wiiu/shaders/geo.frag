#version 330
in vec4 v_col;
in vec4 v_spc;
flat in vec4 f_col;
flat in vec4 f_spc;
in vec2 v_uv;
in float v_w;
out vec4 color;
uniform sampler2D tex0;
// 128 x 2 lookup-table fog: each entry's high byte in row 1, low byte in row 0
uniform sampler2D fog_table;
// x: textured, y: punch-through alpha reference (< 0 = no test), z: use vertex alpha, w: ignore texture alpha
uniform vec4 u_mode;
// x: TSP shading instruction (0 decal, 1 modulate, 2 decal alpha, 3 modulate alpha), y: TSP fog control
// (0 table, 1 vertex, 2 none, 3 table mode 2), z: FOG_DENSITY, w: Gouraud shading (else flat)
uniform vec4 u_shade;
uniform vec4 u_fog_col;    // FOG_COL_RAM, for the table modes
uniform vec4 u_fog_vcol;   // FOG_COL_VERT, for vertex fog
uniform vec4 u_clamp_min;  // FOG_CLAMP_MIN and FOG_CLAMP_MAX: the final colour of polygons with TSP colour
uniform vec4 u_clamp_max;  // clamping is limited to them (u_clamp_on.x)
uniform vec4 u_clamp_on;

// the PVR indexes its fog table by density x W on a log2 scale, 16 entries per octave, and blends the two
// bytes of an entry by the fraction in between (as Flycast does)
float table_fog(float w)
{
    float z = clamp(w * u_shade.z, 1.0, 255.9999);
    float e = floor(log2(z));
    float m = z * 16.0 / exp2(e) - 16.0;
    float idx = floor(m) + e * 16.0 + 0.5;
    return texture(fog_table, vec2(idx / 128.0, 0.75 - fract(m) / 2.0)).r;
}
void main()
{
    bool gouraud = u_shade.w > 0.5;
    vec4 base = gouraud ? v_col : f_col;
    vec4 spc = gouraud ? v_spc : f_spc;
    if (u_mode.z < 0.5)
        base.a = 1.0;
    vec4 c = base;
    if (u_mode.x > 0.5)
    {
        vec4 t = texture(tex0, v_uv);
        if (u_mode.w > 0.5)
            t.a = 1.0;
        int instr = int(u_shade.x);
        if (instr == 0)
            c = t;
        else if (instr == 1)
            c = vec4(t.rgb * base.rgb, t.a);
        else if (instr == 2)
            c = vec4(mix(base.rgb, t.rgb, t.a), base.a);
        else
            c = t * base;
        c.rgb += spc.rgb;
    }
    if (u_mode.y >= 0.0 && c.a < u_mode.y)
        discard;
    int fog = int(u_shade.y);
    if (fog == 0)
        c.rgb = mix(c.rgb, u_fog_col.rgb, table_fog(v_w));
    else if (fog == 1)
        c.rgb = mix(c.rgb, u_fog_vcol.rgb, spc.a);
    else if (fog == 3)
        c = vec4(u_fog_col.rgb, table_fog(v_w));
    if (u_clamp_on.x > 0.5)
        c = clamp(c, u_clamp_min, u_clamp_max);
    color = c;
}
