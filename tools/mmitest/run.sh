#!/bin/zsh
# Builds and runs the MMI translation test on the Mac against a PS2Recomp checkout whose
# recompiler is built in build-mac. Usage: tools/mmitest/run.sh [PS2Recomp dir] [iterations]
set -e
P=${1:-$HOME/tools/PS2Recomp-sotc}
B=$P/build-mac
OUT=${TMPDIR:-/tmp}/pop-mmitest
mkdir -p $OUT
cd ${0:A:h}
c++ -std=gnu++20 -O1 -DUSE_SSE2NEON -I$B/_deps/sse2neon-src -I$P/ps2xRecomp/include -I$B/_deps/elfio-src \
    -I$P/ps2xRuntime/include -I$B/_deps/fmt-src/include -I$B/_deps/toml11-src/include -I$B/_deps/rabbitizer-src/include \
    -flto=thin gen.cpp $B/ps2xRecomp/libps2_recomp_lib.a $B/ps2xRecomp/librabbitizer.a $B/_deps/fmt-build/libfmt.a \
    $B/_deps/libdwarf-build/src/lib/libdwarf/libdwarf.a -lz -o $OUT/mmigen
$OUT/mmigen $OUT/mmi_cases.inc
c++ -std=gnu++20 -O1 -DUSE_SSE2NEON -I$OUT -I$B/_deps/sse2neon-src -I$P/ps2xRuntime/include -I$P/ps2xIOP/include \
    harness.cpp -o $OUT/mmitest
$OUT/mmitest ${2:-20000} || true

# FPU (COP1) instructions against the EE FPU model (fpuharness.cpp).
c++ -std=gnu++20 -O1 -DUSE_SSE2NEON -I$B/_deps/sse2neon-src -I$P/ps2xRecomp/include -I$B/_deps/elfio-src \
    -I$P/ps2xRuntime/include -I$B/_deps/fmt-src/include -I$B/_deps/toml11-src/include -I$B/_deps/rabbitizer-src/include \
    -flto=thin fpugen.cpp $B/ps2xRecomp/libps2_recomp_lib.a $B/ps2xRecomp/librabbitizer.a $B/_deps/fmt-build/libfmt.a \
    $B/_deps/libdwarf-build/src/lib/libdwarf/libdwarf.a -lz -o $OUT/fpugen
$OUT/fpugen $OUT/fpu_cases.inc
c++ -std=gnu++20 -O1 -frounding-math -DUSE_SSE2NEON -I$OUT -I$B/_deps/sse2neon-src -I$P/ps2xRuntime/include -I$P/ps2xIOP/include \
    fpuharness.cpp -o $OUT/fputest
$OUT/fputest ${2:-20000}
