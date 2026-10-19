// Native frames (PNF1..PNF6, written by tools/overrides/pop_native_live.cpp) into a Renderer: images,
// materials (the game's render state flags through materialFromGsFlags), meshes and draws in the
// game's order, plus the frame's post-effect trailers. Shared by pop_native_view (files and the live
// window) and the in-game native world (pop_native_world.cpp).
#pragma once
#include "native_renderer.h"

#include <istream>
#include <unordered_map>

namespace pop_native
{
    struct FrameExtras
    {
        bool glowOn = false;
        uint32_t glow[5] = {}; // ColorDiffusion: levels, FIX, mask, convolution, strength
        bool hazeOn = false;
        uint32_t haze[2] = {}; // SmartBlur: colour, fade (float bits)
        std::vector<Image> gsImages; // live frames: the game's GS pictures (POP_NATIVE_WINDOW_GS)
    };

    class FrameLoader
    {
    public:
        // Reads one frame from `in`: adds its images, materials and meshes to `r` (call r.upload()
        // afterwards) and its draws to `draws`; `clip` receives the frame's world -> clip matrix.
        // PNF6 images sent in an earlier frame are found by texture key (kept until forgetImages).
        // verbose: the one-off reports of the file viewer (glow / haze parameters, counts).
        bool load(Renderer &r, std::istream &in, const char *name, float clip[16], std::vector<Draw> &draws, FrameExtras &extras,
                  bool verbose);
        // a new sender (or a renderer reset without keeping images): no texture keys known
        void forgetImages() { imageOf_.clear(); }

        size_t vertexTotal = 0, triTotal = 0, imageTotal = 0;

    private:
        std::unordered_map<uint32_t, int> imageOf_; // PNF6: texture key -> renderer image
    };

    // true for frames whose draws are in the game's own order (PNF4 and later): draw them in one pass
    bool frameInGameOrder(const char magic[4]);
}
