// Native renderer core (Stage B step 3): draws the game's static world with Vulkan from converted
// meshes, textures and materials, through the game's own world -> clip matrix. Offscreen for now
// (renders to an image and reads it back), so it can be judged against a GS frame; the same core
// will later draw into the game window. Portable: Vulkan 1.1 via volk, no platform APIs.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pop_native
{
    struct Vertex
    {
        float pos[3];
        float uv[2];
        float colour[4]; // vertex colour, already normalised (PS2 0x80 = 1.0)
        float uv2[2];    // lightmap UVs (pass 3), 0 when the mesh has no lightmap
    };

    struct Image
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba; // width * height * 4
    };

    // Blend kinds: the GS ALPHA settings the game's CJadeRenderStateManager::SetBlending (retail
    // 0x4B2500) chooses, (A - B) * C + D with As = source alpha (Ad with Material::destAlpha) on the
    // PS2 scale (0x80 = 1.0).
    enum class AlphaMode
    {
        Opaque = 0,      // 0x29 (FIX 0): Cs
        Mask = 1,        // opaque with the alpha test (legacy form of Opaque + alphaTest)
        Blend = 2,       // 0x44: (Cs - Cd) * As + Cd
        Add = 3,         // 0x68 FIX 0x80: Cs + Cd
        AddAlpha = 4,    // 0x48: Cs * As + Cd
        Subtract = 5,    // 0x62 FIX 0x30: Cd - Cs * fix
        Multiply = 6,    // 0x89: Cd * As
        InvMultiply = 7, // 0x46: Cd * (1 - As)
        InvBlend = 8,    // 0x01: (Cd - Cs) * As + Cs
        AddDest = 9,     // 0x09: Cd * As + Cs
    };

    struct Material
    {
        int baseImage = -1;     // -1: white
        int lightmapImage = -1; // -1: no lightmap
        float lightmapStrength = 1.0f; // darkening = texel * strength (world lightmaps 1; characters' shading pass Af / 128)
        AlphaMode alphaMode = AlphaMode::Opaque;
        float cutoff = 0.5f;
        bool alphaTest = false;     // GS alpha test (always on for Mask): texel alpha > cutoff
        bool alphaTestLess = false; // ATST LESS instead of GREATER (flag 0x80 with the test)
        bool destAlpha = false;     // blend with the destination alpha (flag 0x1000)
        bool depthWrite = true;     // false: ZBUF ZMSK (flag 0x200)
        bool depthTest = true;      // false: ZTST ALWAYS (render state 0x70000038 = 0)
        bool writeRgb = true;       // GS FBMSK: colour written
        bool writeAlpha = false;    // GS FBMSK: alpha (= As) written; the frame's alpha is the glow mask
        float fix = 1.0f;           // FIX / 128 for Subtract
        bool legacyBlend = false;   // glTF BLEND: no depth write, nearly transparent texels dropped
        bool afailRgbOnly = false;  // GS TEST AFAIL RGB_ONLY: alpha-test failures write colour, not depth
        bool doubleSided = true; // false: back faces culled (characters: VU1 culls them)
    };

    inline bool isBlended(AlphaMode m) { return m != AlphaMode::Opaque && m != AlphaMode::Mask; }

    // A sub-material's flags word (record +0x1C; the render state's 0x70000000 at draw time) and its
    // +0x28 word (AREF in the top byte), as the game turns them into GS state (FUN_004B2000 and
    // SetBlending): bits 16-19 = blend kind, 0x10 alpha test (AREF = top byte / 2 on the PS2 scale,
    // ATST GREATER, LESS with 0x80), 0x80 inverse blend, 0x100 AFAIL RGB_ONLY, 0x200 no depth write,
    // 0x1000 destination alpha.
    inline void materialFromGsFlags(uint32_t flags, uint32_t ref, Material &m)
    {
        const bool inv = (flags & 0x80) != 0, dest = (flags & 0x1000) != 0;
        m.destAlpha = false;
        m.fix = 1.0f;
        switch ((flags >> 16) & 0xF)
        {
        case 1: m.alphaMode = inv ? AlphaMode::InvBlend : AlphaMode::Blend; m.destAlpha = dest; break;
        case 2: m.alphaMode = AlphaMode::AddDest; m.destAlpha = dest; break;
        case 3: m.alphaMode = inv ? AlphaMode::InvMultiply : AlphaMode::Multiply; m.destAlpha = dest; break;
        case 4: m.alphaMode = AlphaMode::Add; break;
        case 5: m.alphaMode = AlphaMode::Subtract; m.fix = 0x30 / 128.0f; break;
        case 6: m.alphaMode = AlphaMode::AddAlpha; m.destAlpha = dest; break;
        case 0xF: m.alphaMode = AlphaMode::InvMultiply; break;
        default: m.alphaMode = AlphaMode::Opaque; break;
        }
        m.depthWrite = (flags & 0x200) == 0;
        m.alphaTest = (flags & 0x10) != 0;
        m.alphaTestLess = inv;
        m.cutoff = float((ref >> 24) / 255.0);
        // AFAIL RGB_ONLY: pixels failing the test still write colour, but not depth; the renderer
        // draws such materials twice (passing pixels with depth, failing ones without)
        m.afailRgbOnly = m.alphaTest && (flags & 0x100);
    }

    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        int material = -1;
    };

    // GS fog for the frame (from the VU1 constants the game uploads, e.g. VU 0x10 = (255, 0, a, b)):
    // F = a + b * w clamped to 0..255 (255 = no fog), colour = (F * Cs + (255 - F) * FOGCOL) / 255
    struct Fog
    {
        bool enabled = false;
        float a = 255.0f, b = 0.0f;
        float colour[3] = {0, 0, 0}; // FOGCOL, 0..1
    };

    struct Draw
    {
        int mesh;
        float model[16]; // object -> world, column-major (glTF order)
        bool ownFog = false;           // far layers (sky) use their own GS fog constants
        float fogA = 255.0f, fogB = 0.0f;
        bool ownFogColour = false;     // per draw FOGCOL (live frames: the render state's, 0 for additive blends)
        float fogColour[3] = {0, 0, 0};
    };

    // The game's post effects for the window path (present): CAfterEffect::ColorDiffusion (glow) and
    // AE_ProcessSmartBlur (haze), run on the GPU (shaders gs_sprite / color_diffusion; CPU references
    // and judge in postfx.h / pop_postfx_gpu_judge). Off by default.
    struct PostEffects
    {
        bool glow = false;
        int levels = 6, fix = 128;
        bool mask = true, convolution = true;
        int strength = 0;
        bool haze = false;
        uint32_t hazeColour = 0; // effect object +0x124
        float hazeFade = 0.0f;   // +0x144
    };

    struct Options
    {
        bool linearFilter = true; // bilinear texture filtering (the PS2's usual setting) or nearest
        bool gameOrder = false;   // one pass in the given draw order (live frames, already in the game's order)
        float nearW = 0.05f;      // near plane in clip w (view depth)
        int cull = 0;             // experiment: 0 none, 1 cull clockwise, 2 cull counter-clockwise (all draws)
        bool cullClockwise = true; // single-sided materials: which screen winding is the back face
        float clearColour[4] = {24 / 255.0f, 24 / 255.0f, 32 / 255.0f, 0.0f}; // alpha 0: EraseAlphaBuffer
        // windowed use: the instance extensions the window system needs, and a callback that creates
        // the window's surface for the instance (returns a VkSurfaceKHR handle, 0 on failure)
        std::vector<std::string> instanceExtensions;
        std::function<uint64_t(void *instance)> createSurface;
    };

    class Renderer
    {
    public:
        Renderer();
        ~Renderer();
        Renderer(const Renderer &) = delete;
        Renderer &operator=(const Renderer &) = delete;

        bool init(const Options &options, std::string &error);
        int addImage(const Image &image);
        int addMaterial(const Material &material);
        int addMesh(Mesh mesh);
        // uploads everything added so far (call once, after all add* calls)
        bool upload(std::string &error);
        // drops the scene (materials, meshes, and the images unless keepImages) and their GPU copies
        // so a new one can be added and uploaded; keeps the device, pipelines, window and render
        // targets (live frames). Image indices stay valid while images are kept.
        void reset(bool keepImages = false);
        // renders the draws through `clip` (the game's world -> clip matrix, row vectors as the game
        // stores them) into a width x height RGBA image
        // depthOut (optional): per-pixel view depth w (0 where nothing was drawn)
        bool renderOffscreen(uint32_t width, uint32_t height, const float clip[16], const std::vector<Draw> &draws,
                             std::vector<uint8_t> &rgbaOut, std::string &error, std::vector<float> *depthOut = nullptr);
        // windowed: renders the frame at internalWidth x internalHeight and shows it scaled into the
        // window (aspect kept, bars around it); recreates the swapchain when the window size changes
        bool present(uint32_t windowWidth, uint32_t windowHeight, uint32_t internalWidth, uint32_t internalHeight,
                     const float clip[16], const std::vector<Draw> &draws, std::string &error);
        void setFog(const Fog &fog);
        void setPostEffects(const PostEffects &post);
        // an image to present instead of the scene (e.g. the game's own GS picture, for comparing);
        // nullptr: present the scene again. Copied; same-size updates reuse the texture.
        bool setOverlay(const Image *image, std::string &error);
        // the last frame present() drew (internal resolution, after the post effects), read back
        bool readPresented(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, std::string &error);
        std::string deviceName() const;

    private:
        struct Impl;
        Impl *m;
    };
}
