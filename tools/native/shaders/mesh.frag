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

// The water's 256x256 refraction buffer (FUN_004A6F40; docs/native_renderer.md "The refraction copy"), one texel at
// GS pixel p, from the frame copy in baseTex: cleared to 0, then five copies of the 640x448 frame (box = the copy's
// pixels, map: u = x * m.x + m.y, v = y * m.z + m.w in frame pixels), tinted (0x67, 0x70, 0x7F), ALPHA 0x44, kept only
// where the texel alpha passes GEQUAL 0x7F (the mask pass wrote 0x80); the buffer's alpha is the copy's As.
vec4 refractionTexel(vec2 p)
{
    const vec4 box[5] = vec4[5](vec4(2, 2, 258, 258), vec4(2, 0, 258, 254), vec4(0, 2, 254, 258), vec4(0, 0, 254, 254), vec4(0, 0, 256, 256));
    const vec4 map[5] = vec4[5](vec4(2.5, -5.0, 1.75, -3.5), vec4(2.5, -5.0, 445.0 / 254.0, 3.0), vec4(637.0 / 254.0, 3.0, 1.75, -3.5),
                                vec4(2.5, 5.0, 445.0 / 254.0, 3.0), vec4(2.5, 0.0, 1.75, 0.0));
    vec4 c = vec4(0.0);
    for (int k = 0; k < 5; ++k)
    {
        if (any(lessThan(p, box[k].xy)) || any(greaterThanEqual(p, box[k].zw)))
            continue;
        vec4 t = texture(baseTex, vec2(p.x * map[k].x + map[k].y, p.y * map[k].z + map[k].w) / vec2(640.0, 448.0));
        float as = floor(t.a * 128.0 + 0.5); // the frame's alpha: 1.0 = 0x80
        if (as < 127.0)
            continue;
        c.rgb += (t.rgb * vec3(103.0, 112.0, 127.0) / 128.0 - c.rgb) * as / 128.0;
        c.a = as / 128.0;
    }
    return c;
}

// the colour pass's bilinear read of that buffer (TEX1 0x60, CLAMP 5) at normalised ST
vec4 refraction(vec2 st)
{
    vec2 x = st * 256.0 - 0.5, i = floor(x), f = x - i;
    vec2 i0 = clamp(i, 0.0, 255.0), i1 = clamp(i + 1.0, 0.0, 255.0);
    return mix(mix(refractionTexel(i0), refractionTexel(vec2(i1.x, i0.y)), f.x),
               mix(refractionTexel(vec2(i0.x, i1.y)), refractionTexel(i1), f.x), f.y);
}

void main()
{
    int flags = int(pc.params.x + 0.5);
    // bit 256: a halo's visibility (FUN_004AB500): the GS halves the core square bilinearly down to one pixel and spreads it
    // with As = At * Av; here the mean of the frame copy's alpha over the square (vUv .. vUv2), 8x8 samples
    if ((flags & 256) != 0) {
        float s = 0.0;
        for (int j = 0; j < 8; ++j)
            for (int i = 0; i < 8; ++i)
                s += texture(baseTex, mix(vUv, vUv2, (vec2(i, j) + 0.5) / 8.0)).a;
        outColour = vec4(0.0, 0.0, 0.0, s / 64.0 * vColour.a);
        return;
    }
    // bit 128: the water's colour pass, baseTex = the frame copy read through the refraction buffer
    vec4 t = (flags & 128) != 0 ? refraction(vUv) : texture(baseTex, vUv);
    // GS MODULATE with TCC: As = texel alpha * vertex alpha (both 0x80 = 1.0); the blend state
    // (pipeline) uses it as the GS ALPHA C term, and it is the alpha written where FBMSK allows.
    float a = t.a * min(vColour.a, 1.0);
    // params.x bits: 1 alpha test (GREATER cutoff; LESS with bit 4; bit 16 keeps the failures instead),
    // 2 blended, 8 legacy glTF blend, 32 fog as vertex alpha
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
    // GS fog: (F * Cs + (255 - F) * FOGCOL) / 255, applied before alpha blending; bit 32: F is the vertex
    // alpha instead (lightmap pass: P1 mode 3 writes clamp(c + d * w) as RGBAQ alpha, PRMODE without fog)
    if ((flags & 32) != 0)
        a *= vFog * 255.0 / 128.0;
    else
        base = mix(pc.fogColour.rgb, base, vFog);
    outColour = vec4(base, a);
}
