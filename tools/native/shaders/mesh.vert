#version 450
// Native static-world pass. The game's world -> clip matrix uses the PS2 conventions: screen y
// grows downwards (as Vulkan's), w = view depth. Depth is zScale * (w - z) / w: the GS Z the VU1
// programs compute (viewport 0x280: Z = 2^23 * (1 - z / w)) scaled by zScale (the game's z - w is
// about -0.2, so depth ~ 0.2 * zScale / w; reversed: nearer = larger, cleared to 0, compare
// GREATER), and Vulkan's 0 <= z <= w clip does the near-plane clipping at w ~ 0.2 * zScale.
layout(push_constant) uniform Push {
    mat4 mvp;          // game clip matrix * object matrix (column-major)
    vec4 params;       // x: flags (1 alpha test, 2 blended, 4 test LESS, 8 legacy blend), y: cutoff, z: lightmap strength, w: zScale
    vec4 fog;          // GS fog factor F = x + y * w clamped to z..w (the VU1 drivers' min, max; 255 = no fog)
    vec4 fogColour;    // FOGCOL (0..1); a: depth bias of draws without depth write, GS Z units
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
// lightmaps and shadow receivers redraw their base with GEQUAL, so every pipeline must give bit-identical depth (N18)
invariant gl_Position;
void main()
{
    vec4 c = pc.mvp * vec4(inPos, 1.0);
    // params.x bit 64 (no depth write): fogColour.a GS Z units (2^-25 of the depth each) nearer. A decal passes GEQUAL over
    // its wall on the GS; our Z differs from VU1's by tens of units, so with float depth it flipped between frames (N16 moss)
    float bias = (int(pc.params.x + 0.5) & 64) != 0 ? pc.fogColour.a / 33554432.0 : 0.0;
    gl_Position = vec4(c.x, c.y, pc.params.w * (c.w - c.z) + bias * c.w, c.w);
    vUv = inUv;
    vColour = inColour;
    vUv2 = inUv2;
    vFog = clamp(pc.fog.x + pc.fog.y * c.w, pc.fog.z, pc.fog.w) / 255.0;
}
