// Game overrides for Prince of Persia: The Sands of Time, US v1.01 (SLUS_207.43).
// rebuild.sh copies this into ps2xRuntime/src/runner/ so it links into the
// executable: overrides compiled into the ps2_runtime static library are
// dropped by the linker and never apply (upstream issue #264).

#include "game_overrides.h"

#include <cstdint>

namespace
{
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kElfCrc32 = 0x7E7889A9u; // whole-file CRC-32 of SLUS_207.43

    // sceCdLayerSearchFile(sceCdlFILE *fp, const char *name, int layer). The game's
    // own copy waits on the IOP CD driver (and is flagged as self-modifying), so it
    // spins forever. The runtime's sceCdSearchFile takes the same fp/name in a0/a1;
    // the layer in a2 does not matter for this single-layer disc.
    constexpr uint32_t kSceCdLayerSearchFile = 0x00101FA8u;

    // sceCdDiskReady(int mode), not matched by ps2_analyzer's signature scan. Identified
    // from its body: binds the cdvdfsv "disk ready" RPC server 0x8000059A, returns 6
    // (not ready) on failure. Without an IOP CD server the bind loop never ends.
    constexpr uint32_t kSceCdDiskReady = 0x00102A48u;

    void applyPopOverrides(PS2Runtime &runtime)
    {
        ps2_game_overrides::bindAddressHandler(runtime, kSceCdLayerSearchFile, "sceCdSearchFile");
        ps2_game_overrides::bindAddressHandler(runtime, kSceCdDiskReady, "sceCdDiskReady");
    }
}

PS2_REGISTER_GAME_OVERRIDE("Prince of Persia: The Sands of Time (NTSC-U v1.01)",
                           "SLUS_207.43", kEntry, kElfCrc32, applyPopOverrides)
