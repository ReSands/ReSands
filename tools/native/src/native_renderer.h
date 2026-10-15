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

    enum class AlphaMode
    {
        Opaque = 0,
        Mask = 1,  // GS alpha test (AREF)
        Blend = 2, // GS ALPHA (Cs - Cd) * As + Cd, drawn after the opaque pass in draw order
    };

    struct Material
    {
        int baseImage = -1;     // -1: white
        int lightmapImage = -1; // -1: no lightmap
        AlphaMode alphaMode = AlphaMode::Opaque;
        float cutoff = 0.5f;
    };

    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        int material = -1;
    };

    struct Draw
    {
        int mesh;
        float model[16]; // object -> world, column-major (glTF order)
    };

    struct Options
    {
        bool linearFilter = true; // bilinear texture filtering (the PS2's usual setting) or nearest
        float nearW = 0.05f;      // near plane in clip w (view depth)
        float clearColour[4] = {24 / 255.0f, 24 / 255.0f, 32 / 255.0f, 1.0f};
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
        // renders the draws through `clip` (the game's world -> clip matrix, row vectors as the game
        // stores them) into a width x height RGBA image
        // depthOut (optional): per-pixel view depth w (0 where nothing was drawn)
        bool renderOffscreen(uint32_t width, uint32_t height, const float clip[16], const std::vector<Draw> &draws,
                             std::vector<uint8_t> &rgbaOut, std::string &error, std::vector<float> *depthOut = nullptr);
        // windowed: renders the frame at internalWidth x internalHeight and shows it scaled into the
        // window (aspect kept, bars around it); recreates the swapchain when the window size changes
        bool present(uint32_t windowWidth, uint32_t windowHeight, uint32_t internalWidth, uint32_t internalHeight,
                     const float clip[16], const std::vector<Draw> &draws, std::string &error);
        std::string deviceName() const;

    private:
        struct Impl;
        Impl *m;
    };
}
