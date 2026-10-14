You are translating a Ghidra decompilation of a PlayStation 2 game's geometry loader (C) into Python, piece by piece.

Context: the function reads a geometry object from a byte buffer. We want a Python reader with exactly the same read sequence, so it can parse the same file data.

Rules (follow exactly):
1. Translate ONLY the C lines given in the CHUNK, statement by statement, in order. Do not invent code, do not skip statements, do not summarise.
2. Keep the block structure. The chunk starts at Python nesting depth DEPTH (indent = 4 spaces x DEPTH). Each C `{` that opens an if/else/for/while/do body adds one level; each `}` closes one. Emit Python lines at the right indentation. A chunk may start or end in the middle of a block: just keep indenting consistently. Never close or reopen blocks the chunk does not contain.
3. Mappings:
   - Reading one 32-bit word from the buffer: the pattern `x = *puStack_f0; puStack_f0 = puStack_f0 + 1;` (or `*puStack_f0` then `puStack_f0 + 1`) becomes `x = r.u32()`. `puStack_f0[1]` read then `puStack_f0 + 2` means two reads: emit them as consecutive `r.u32()` calls in order. `puStack_f0 = puStack_f0 + N` with no read becomes `r.skip(4*N)`. `(int)puStack_f0 + iVar` style advances become `r.skip(iVar)`.
   - `_LOA_ReadVector(&puStack_f0, dst, n)` becomes `r.vec3()` when n == 1 (store the result if dst is a field), and `r.skip(12)` when dst is 0 or n == 2 (n == 2 means read-and-discard).
   - `_LOA_ReadLong(...)` becomes `r.u32()`.
   - Writes into the object `*(T *)(iVar28 + OFF) = v` become `obj[OFF] = v` (OFF as hex). Reads `*(int *)(iVar28 + OFF)` become `obj[OFF]`.
   - The speed-mode test `(_LOA_gb_SpeedMode != 0) && (_BIG_gi_ReadMode == 2)` is always true: use the constant `SPEED` (True).
   - `param_4` is the format version: use the variable `version`.
   - Any other function call becomes `call('Name', args...)` keeping its arguments, except: `GEO_SKN_Load(puStack_f0, ...)` becomes `n = call('GEO_SKN_Load', r)` and the following `puStack_f0 = puStack_f0 + n` becomes `r.skip(n)`; allocations (`(**(code **)(... + 0xc))(...)`, `InitAllocator...`) become `None`.
   - Keep local variable names as they are (uVar12, iVar9, ...). Casts disappear. `(int)x` etc. become `x`. Unsigned 32-bit arithmetic: wrap with `& 0xffffffff` only where the C clearly relies on overflow.
   - `goto LABEL;` becomes `# goto LABEL` on its own line (we fix these by hand). Labels `LABEL:` become `# LABEL:`.
4. Output: ONLY the Python lines for this chunk inside one ```python block. No explanations.
