// Shadow map vertex shader — depth-only pass rendering world geometry from
// the sun's point of view. No fragment shader.
// Ported from Quake 2 RTX (shadow_map.vert), GPL v2.

#version 460

layout(location = 0) in vec4 world_pos;

layout(push_constant, std140) uniform ShadowMapConstants
{
    mat4 view_projection_matrix;
} push;

void main()
{
    gl_Position = push.view_projection_matrix * vec4(world_pos.xyz, 1.0);
    gl_Position.y = -gl_Position.y;
}
