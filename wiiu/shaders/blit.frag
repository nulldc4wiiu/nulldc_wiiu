#version 330
in vec2 v_uv;
out vec4 color;
uniform sampler2D tex0;
void main()
{
    color = texture(tex0, v_uv);
}
