#!/usr/bin/env bash
# Builds the "pop-sotc" branch of a PS2Recomp clone: TaylorNAlbarnaz's sotc-port fork (the
# runtime behind the Shadow of the Colossus port: GPU GS renderer, SPU2, async VIF/GS, VU1
# microcode compiler) at a fixed commit, plus our patches (tools/patches/pop-sotc/) and two
# upstream PRs that merge cleanly. Windows/x86 only: the fork includes x86 SSE headers directly.
# Usage: PS2RECOMP=/c/tools/PS2Recomp-sotc bash tools/ps2recomp-pop-sotc-branch.sh   (Git Bash)
set -euo pipefail

REPO=${PS2RECOMP:-$HOME/tools/PS2Recomp-sotc}
BASE=ac9efa0   # TaylorNAlbarnaz/PS2Recomp sotc-port, 12 Oct 2026
PATCHES=$(cd "$(dirname "$0")/patches/pop-sotc" && pwd)

cd "$REPO"
git remote get-url sotc >/dev/null 2>&1 || git remote add sotc https://github.com/TaylorNAlbarnaz/PS2Recomp
git fetch -q sotc sotc-port
git fetch -q origin '+refs/pull/235/head:refs/remotes/pr/235' '+refs/pull/279/head:refs/remotes/pr/279'
git checkout -q -B pop-sotc $BASE

git apply "$PATCHES/01-cop0-count.patch" && git commit -q -am "COP0 Count advances with host time at the EE clock"
for n in 235 279; do git merge -q --no-ff --no-edit -m "Merge upstream PR #$n" pr/$n >/dev/null; done
git apply "$PATCHES/02-vcallmsr-cmsar0.patch" && git commit -q -am "VCALLMSR reads CMSAR0 (as upstream PR #268)"
git apply "$PATCHES/03-runner-vu1-generated.patch" && git commit -q -am "Runner: build compiled VU1 programs from src/vu1_generated"
echo "pop-sotc = $(git rev-parse --short HEAD)"
