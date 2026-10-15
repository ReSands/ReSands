#version 450
// PS2 MODULATE (texture * vertex colour, 0x80 = 1.0, already normalised by the converter) and the
// lightmap pass folded in as the best frame match so far ("lerpvc", tools/gltfpreview.py): the game
// draws the grey lightmap texel (CLUT alpha As = texel) with ALPHA ((Cs - Cd) * As >> 7) + Cd.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 params;
} pc;
layout(set = 0, binding = 0) uniform sampler2D baseTex;
layout(set = 0, binding = 1) uniform sampler2D lightmapTex;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColour;
layout(location = 2) in vec2 vUv2;
layout(location = 0) out vec4 outColour;
void main()
{
    vec4 t = texture(baseTex, vUv);
    if (pc.params.x == 1.0 && t.a < pc.params.y)
        discard;
    if (pc.params.x == 2.0 && t.a < 0.02)
        discard;
    vec3 base = t.rgb * vColour.rgb;
    if (pc.params.z > 0.5) {
        float a = texture(lightmapTex, vUv2).r;
        base = base * (1.0 - a) + a * a * vColour.rgb;
    }
    outColour = vec4(base, pc.params.x == 2.0 ? t.a : 1.0);
}
