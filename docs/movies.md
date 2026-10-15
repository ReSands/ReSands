# PS2 movie files (.INT): format and the native-decoder plan

Research 15 Oct 2026 (user: 1.0 needs a native decoder for the PS2 movie files, done as staged
local-model work). Sources: the August 2003 prototype's player (`FullMovie`, `VideoData`,
`SoundData`, Jade's `VID_` module), decompiled to `game/analysis/movie/decomp/<proto addr>.c`, and
the disc files. Nothing here comes from Sony SDK material.

## Files
`*.INT` on the disc root (UBISOFT, POPLOGO, INTRO, POP_IN/OUT, CINE_010/110/730/750/820/830) and
`MOVIEPS2.BIG` (+ `.FAT`, not looked at yet). FFmpeg does not read the container ("Invalid data"),
but FFmpeg 9 has an **IPU video decoder** (`-codecs`: `ipu`) and a raw-IPU demuxer: the judge for
the video path.

## Container (FullMovie::mReadHeaders, proto 0x19CD00)
Main header, 0x28 bytes, little-endian:

| off | meaning | UBISOFT | CINE_010 |
|---|---|---|---|
| 0x00 | buffer size? (not read into the object) | 0x200000 | |
| 0x04 | frame rate (float) | 29.97 | 29.97 |
| 0x08 | object +0x35: frames per chunk group? (`last chunk frames = frames - this * (chunks - 1)`) | 5 | 5 |
| 0x0C | chunk count N | 36 | 572 |
| 0x10 | sound track count T | 1 | 5 (languages) |
| 0x14..0x17 | flags: video, alpha, sound, subtitles | 1,0,1,0 | 1,0,1,0 |
| 0x18 | video header size | 0x344 | 0x2D24 |
| 0x1C | alpha header size | 0 | 0 |
| 0x20 | sound header size (each track) | 0xA8 | 0x908 |
| 0x24 | subtitle size | 0 | 0 |

Then, read in one block of `(N+1)*4 + video + alpha + sound*T + subtitles` bytes:
1. chunk position table, (N+1) u32 (copied to the object; relative positions);
2. video header (`VideoData::mReadHeader` -> `VID_fn_iLoadMovieFromBuffer`): u32 version, must be
   3; then `VID_tdstMovieInfo`, 0x80 bytes (seen: +0x60 width in macroblocks (40 = 640),
   +0x64 height in macroblocks (28 = 448, 22 = 352 letterboxed), +0x68 1.0f, +0x6C index present,
   +0x70 index count, +0x74/+0x78 sizes?; the IPU quantiser matrices sit in the first part);
   then the index: count u32 (`VID_fn_p_iLoadIndexesFromBuffer`);
3. alpha header (unused here); 4. per sound track (`SoundData::mReadHeader`): 0x14 bytes of fields
   (`+4 == 2` = ADPCM, `mIsSoundStereo` reads another) then (N+1) u32 sub-chunk positions
   (`mGetSubChunkPos/Size`: audio slice of each chunk); 5. subtitles (unused).

Still to read from the decompiles before writing the spec: how chunk positions turn into file
offsets (`mStreamData`, `mStreamingMoveToNextChunk`, `mSetCurrentFrameDataPos`), where video data
sits inside a chunk and how frames are split (`VID_fn_vSetDecodingSource`, `VID_fn_vDoDecodeMovie`:
the bytes handed to the IPU), and the audio layout (`mQueueSoundData`, mono/stereo ADPCM; PS2
SPU ADPCM = VAG, 16-byte blocks, which vgmstream and FFmpeg decode: a second judge).

## Chunk contents (found 15 Oct, for stage 2)
- Frame k's IPU data is `index[k+1] - index[k]` bytes (last frame: up to the end of the chunk's
  video part) at `index[k] - index[first frame of the chunk]` inside the chunk's video part
  (`VideoData::mGetSubChunkSize`); chunk c holds frames c*framesPerChunk .. +framesPerChunk-1
  (framesPerChunk = header +0x08 = 5); chunk c's data at `dataStart + chunks[c]`.
- Sound header fields: (0x16f0, 1, 16 bits, 2 channels, 48000 Hz): **raw s16 stereo PCM**, not
  ADPCM (0x7D20 bytes per chunk = 8,008 stereo samples = 5 frames at 29.97); track t's slice of
  chunk c is `subChunks[c+1] - subChunks[c]` bytes.
- chunk size = video + all tracks' slices + 0 or 32 bytes. Open: the order inside a chunk (sound
  first or video first) and where the 32 bytes go; stage 2's reference must settle it by decoding
  (FFmpeg IPU must accept the frames; PCM must sound right).

## Frames and the IPU (15 Oct, stage 2 research)
- Chunk data base = the header end rounded up to 64 bytes (UBISOFT: 0x4A8 -> 0x4C0, i.e. +0x18):
  frame k = `[base + chunks[c] + index[k] - index[c*fpc], ... + index[k+1] - index[c*fpc])`, video first
  in the chunk, each frame ending with zero padding to 16 bytes and the code `00 00 01 B0`; the sound
  follows the chunk's video (raw s16 stereo PCM, 48 kHz).
- The player (`VID_fn_vSetDecodingContext`, `VID_fn_vDecodeMovie`): IPU SETTH (0x9...), IPU_CTRL from
  the movie's decoding settings (`VID_fn_vSetCTRLValue`: DCPR, AS, IVF, QST, MP1 = FFmpeg's IPU flag
  bits), then **IDEC** (command 1) with QSC/DTD/SGN/DTE/OFM from the movie object; source DMA of the
  frame rounded to 16 bytes.
- Frames start with bytes like 0x4B, 0x47, 0x45, 0x43 (0x40 always set).
- **FFmpeg's IPU decoder rejects every variant tried**: as stored, padding stripped before the end
  code, any prepended flag byte (all 256), 32-bit word-swapped. FFmpeg expects flag byte + IDEC
  macroblocks + exactly 32 bits after byte alignment. So the game's IDEC stream differs from Sony's
  `.ipu` files in some detail not found yet.
- Next: decode one frame with a hardware-faithful IPU model instead of FFmpeg: ps2-web-recomp's MIT IPU
  (our chosen IPU base, `runtime/src/ipu/ipu.cpp`) run in IDEC mode on frame 0 with the game's CTRL/CMD
  values; that tells exactly what the stream is, and gives the judge for stage 2 (or a conversion to
  FFmpeg's format).

## Stage 1 kit (ready)
`tools/llm_port/movie/`: `movie.h` (fixed API), `reference.py` (writes `game/analysis/movie/tests/`:
first 1 MB of each movie + a 0x60-byte truncated copy that must give false), `driver.cpp`,
`judge.py` (validated: a correct parser passes 22/22), `port.py`. Run: `tools/qwen-stage.command m1`.

## Plan for the local model (same method as the mesh decoder: small stages, strict judge)
Each stage: spec = the decompiled C + these notes; a Python reference (written by Claude, short)
defines the exact output; Qwen ports it to C++ against a fixed header; the judge compares outputs
on all 11 movies.
1. **Header parser**: main header, chunk table, sound headers, movie info + index -> canonical text.
   Judge: identical text (Python reference).
2. **Chunk/frame extraction**: for every frame, the raw IPU bytes (as `VID_fn_vSetDecodingSource`
   gets them). Judge: Python reference bytes; plus FFmpeg's IPU decoder decodes the concatenated
   stream (wrapped as an `.ipu` file) without errors and with the expected frame count.
3. **Audio extraction**: per track, the ADPCM bytes per chunk. Judge: Python reference bytes; decoded
   with vgmstream/FFmpeg as PCM, compared by hash.
4. **Decoder glue** (C++, not Qwen): FFmpeg's IPU decoder + ADPCM decode behind the game's
   `FullMovie` hooks (where `tools/overrides/pop_movies.cpp` now plays Bink), so the disc's own
   movies play natively. Judge: frame hashes vs FFmpeg on the extracted stream; A/V sync by eye.

Open question for 1.0: FFmpeg's IPU decoder is LGPL (fine to link, as we already link FFmpeg); a
fully own decoder is not planned (thousands of lines, GPL-contamination risk if written from memory).
