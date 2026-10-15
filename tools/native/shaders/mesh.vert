#version 450
// Native static-world pass. The game's world -> clip matrix uses the PS2 conventions: screen y
// grows downwards (as Vulkan's), w = view depth. Depth is near / w (reversed: nearer = larger,
// cleared to 0, compare GREATER), so Vulkan's 0 <= z <= w clip does the near-plane clipping.
layout(push_constant) uniform Push {
    mat4 mvp;          // game clip matrix * object matrix (column-major)
    vec4 params;       // x: alpha mode (0 opaque, 1 mask, 2 blend), y: cutoff, z: has lightmap, w: near
    vec4 fog;          // x, y: GS fog factor F = x + y * w (clamped 0..255, 255 = no fog), z: enabled
    vec4 fogColour;    // FOGCOL (0..1)
} pc;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColour;
layout(location = 3) in vec2 inUv2;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColour;
layout(location = 2) out vec2 vUv2;
// F as the GS uses it: per vertex, interpolated linearly in screen space (Gouraud), not perspective-correct
layout(location = 3) noperspective out float vFog;
void main()
{
    vec4 c = pc.mvp * vec4(inPos, 1.0);
    gl_Position = vec4(c.x, c.y, pc.params.w, c.w);
    vUv = inUv;
    vColour = inColour;
    vUv2 = inUv2;
    vFog = pc.fog.z > 0.5 ? clamp(pc.fog.x + pc.fog.y * c.w, 0.0, 255.0) / 255.0 : 1.0;
}
