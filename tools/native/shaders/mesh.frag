#version 450
// PS2 MODULATE (texture * vertex colour, 0x80 = 1.0, already normalised by the converter), then the
// lightmap pass as the game draws it (frame capture, 15 Oct): GS ALPHA 0x46 = Cd * (1 - As) with the
// 4-bit lightmap page, whose CLUT alpha ramps 0..0x80 with the index (= our grey / 255). The
// lightmap only darkens what the base pass wrote; its colour (Cs) is not used. Then the GS fog
// (per-vertex F from the VU1 fog constants, e.g. 264.87 - 2.3757 * w on the ramparts).
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 params;
    vec4 fog;
    vec4 fogColour;
} pc;
layout(set = 0, binding = 0) uniform sampler2D baseTex;
layout(set = 0, binding = 1) uniform sampler2D lightmapTex;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColour;
layout(location = 2) in vec2 vUv2;
layout(location = 3) noperspective in float vFog;
layout(location = 0) out vec4 outColour;
void main()
{
    vec4 t = texture(baseTex, vUv);
    // GS MODULATE with TCC: As = texel alpha * vertex alpha (both 0x80 = 1.0); the blend state
    // (pipeline) uses it as the GS ALPHA C term, and it is the alpha written where FBMSK allows.
    float a = t.a * min(vColour.a, 1.0);
    // params.x bits: 1 alpha test (GREATER cutoff; LESS with bit 4; bit 16 keeps the failures instead),
    // 2 blended, 8 legacy glTF blend
    int flags = int(pc.params.x + 0.5);
    if ((flags & 1) != 0) {
        // on the PS2 scale, as the GS: texel alpha is stored doubled (0x80 = 255), AREF = cutoff byte / 2;
        // As = At * Av >> 7 clamped to 0xFF (instanced draws keep Av up to 2.0, the other decoders cap it at 0x80: docs/renderer_audit.md item 9)
        float as = min(floor(floor(t.a * 127.5 + 0.5) * vColour.a + 0.001), 255.0);
        int top = int(pc.params.y * 255.0 + 0.5);
        float aref = float(top >> 1);
        // FUN_004B2000 ORs the byte in at bit 3, so an odd byte's low bit lands in ATST: LESS (2) becomes GREATER (6)
        bool pass = (flags & 4) != 0 && (top & 1) == 0 ? as < aref : as > aref;
        if (pass == ((flags & 16) != 0))
            discard;
    }
    if ((flags & 8) != 0 && t.a < 0.02)
        discard;
    vec3 base = t.rgb * vColour.rgb;
    // params.z: lightmap strength (0 = none); characters' shading pass uses the same blend with
    // As = texel alpha * Af / 128 (VU1 program 0x4DD300 mode 3), so strength = Af / 128
    if (pc.params.z > 0.0) {
        float a = texture(lightmapTex, vUv2).r * pc.params.z;
        base *= 1.0 - a;
    }
    // GS fog: (F * Cs + (255 - F) * FOGCOL) / 255, applied before alpha blending
    base = mix(pc.fogColour.rgb, base, vFog);
    outColour = vec4(base, a);
}
