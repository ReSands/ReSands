# Windows PC setup

Goal: Claude Code runs on the PC (Ryzen 7 5800X) and does the build/boot loop
there; you drive it from the Mac with Remote Control. Code moves between the
machines with git over SSH on the LAN or Tailscale, never via a cloud service.
Game data is copied once and stays out of git.

Paths on the PC: project `C:\dev\PoP_Decomp`, tools `C:\tools` (short paths
matter: there are about 56,000 generated files).

## 1. On the Mac: let the PC reach the repo and game files

- System Settings > General > Sharing > **Remote Login**: on, for your user only.
- Note the Mac's hostname (`scutil --get LocalHostName`, then use `<name>.local`)
  or its Tailscale name.

## 2. On the PC: run the setup script

Copy `tools\windows\setup.ps1` across (or type it in), then in an **elevated**
PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\setup.ps1
```

It installs Git, CMake, Ninja, Java 21, Python, Tailscale and the MSVC Build
Tools; enables OpenSSH Server and long paths; adds Defender exclusions for
`C:\tools` and `C:\dev\PoP_Decomp`; downloads Ghidra and the EE plugin and checks
their SHA-256 against the values verified on the Mac. Ghidra ships Windows native
binaries, so no native build is needed (unlike on the Mac).

Reboot afterwards (long paths and PATH changes).

## 3. On the PC: clone our repo from the Mac

```powershell
git clone ssh://jon@<mac>.local/Users/jon/Projects/PoP_Decomp C:\dev\PoP_Decomp
```

Day to day: commit on one machine, `git pull` on the other. The Mac stays the
origin; nothing is pushed anywhere else.

## 4. On the PC: copy the game data (once)

Only the extracted disc and the Ghidra project are needed. From PowerShell on the PC:

```powershell
cd C:\dev\PoP_Decomp
mkdir game\ps2, game\ghidra
scp -r jon@<mac>.local:Projects/PoP_Decomp/game/ps2/disc game/ps2/
scp -r jon@<mac>.local:Projects/PoP_Decomp/game/ghidra/PoP_SLUS20743.gpr jon@<mac>.local:Projects/PoP_Decomp/game/ghidra/PoP_SLUS20743.rep game/ghidra/
```

`game\` is git-ignored on the PC too. (The Ghidra project could also be rebuilt from
scratch with the import step in CLAUDE.md plus `rebuild`; copying is quicker.)

## 5. On the PC: PS2Recomp with our `pop` branch

```powershell
cd C:\tools
git clone https://github.com/ran-j/PS2Recomp.git
```

Then build the `pop` branch the same way as on the Mac. This step is left for
Claude Code on the PC, together with porting `tools/rebuild.sh` and
`tools/boot.sh` to PowerShell; both need the MSVC environment and Windows paths
worked out on the machine itself.

## 6. On the PC: Claude Code

- Install Claude Code for Windows (see the official install instructions) and log in.
- Start it in `C:\dev\PoP_Decomp`; it reads `CLAUDE.md` there and has the full
  history of decisions and fixes.
- Run `/remote-control` to drive the session from the Mac (claude.ai or the Claude
  app). To *see* the game window, use Moonlight + Sunshine or Parsec.
- Recreate the permission rules for the PowerShell build/boot scripts once they
  exist (`.claude/settings.local.json` is per machine and not in git).

## 7. First tasks for the Windows session

1. Build the `pop` branch with MSVC and run the first full rebuild; note the
   per-stage timings and compare with the Mac (about 10 min full, about 7 of them
   the runtime build).
2. Boot and compare with Mac boot 8: it should stop at the game's own C++
   exception (`exception.cpp` line 35). Investigate what throws.
