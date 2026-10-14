# Jobs for the local model (overnight, unattended)

Rule: a job is only worth running if a script can judge each answer. Without a judge the model
produces plausible nonsense (naming test, 14 Oct: ~1/3 right meaning, 0 exact). Sorted by judge quality.

| # | Job | Input | Judge (automatic) | Value | Ready? |
|---|-----|-------|-------------------|-------|--------|
| 1 | **Fix the remaining wrong MMI translations** (22: PMADD*/PMSUB*/PHM*, PDIV*, PMTHI/PMTLO, PMTHL, PABS*, PADDUH/PSUBUH) | PS2Recomp translator source + the instruction's reference in tools/mmitest/harness.cpp | `tools/mmitest/run.sh`: the instruction passes 20k random cases, no other instruction regresses | Upstream PR; any game using them | Needs a driver that applies a patch, rebuilds, runs the test (Mac, ~1 min per try) |
| 2 | **Reference semantics for VU0 macro (COP2) ops** to extend the instruction test | Public instruction descriptions | Agreement with an independent oracle (PCSX2 interpreter run locally as a test oracle only, never shipped) | Finds the next class of recompiler bugs (VU0 maths: matrices, medallion drift) | Needs the oracle harness |
| 3 | **Other bigfile record formats** (materials, game objects/placement, cameras, animation) | Record bytes + record size + any decompiled loader | Parse consumes exactly the record's size, for every record of that type across all packs; values in sane ranges | Route D: materials and world placement | Needs per-type record lists (jadetex) and a grammar format the model can emit |
| 4 | **World placement of meshes** | Candidate matrices from game-object records | Placed objects form connected buildings (shared edges/vertices), match the GS screenshot of a save-state spot | Route D stage 1 | After #3 finds the game-object format |
| 5 | **Decompile -> Python ports** (c2py gaps, hand-written functions like `_LOA_ReadMatrix`) | Ghidra decompile | Record/replay: recorded inputs/outputs from the running game (as `POP_CAPTURE_ADPCM`) | Faster porting of loaders/mixers | Capture wrappers per function |
| 6 | **Texture assignment per element** | Element UVs, material records, texture keys | UVs fit the texture size; rendered element matches the GS frame | Route D textures | After #3 |
| 7 | **Function naming** (running now) | Decompiles | Calibration against hidden prototype names (meaning, not exact text) | Navigation; weak | Running; verdict pending rescore |
| 8 | **Log triage** | Long boot logs | Diff against a known-good baseline log (new warnings, new missing targets) | Spot regressions after rebuilds | Easy; mostly a script, model only to summarise |

Notes
- The PC's GPU is only free when no build or game runs; one job at a time.
- Each job writes one result file per item and resumes after interruption (like name_functions.py).
- Results are suggestions until the judge passes; never applied to the build automatically.
