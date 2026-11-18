# Hires Textures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Every demo material packs a 1024 px layer built from `terrain_textures_vol2` (or the slop pack where it already is the source), and three slop materials join the set at ids 14–16.

**Architecture:** One constant in the C++ packer moves the array size; `make_textures.py` becomes the single vol2 build script for twelve material sets at 1024 and `organize_material.sh` imports the slop pack at the same size; `materials.tres` gains three defs. The shader, the packing path and the fourteen existing ids are untouched.

**Tech Stack:** C++17, Godot 4.7 GDExtension, SCons, GDScript tests, Python 3 + Pillow, ImageMagick for the slop import.

**Spec:** `docs/superpowers/specs/2026-11-17-hires-textures-design.md`

## Global Constraints

- `TEXTURE_ARRAY_SIZE` = 1024, written once in `addons/dk_voxel/src/core/voxel_materials.cpp`, mirrored by `demo/textures/make_textures.py` and `addons/dk_voxel/tools/organize_material.sh`; `tests/run_tests.gd` asserts the packed size.
- vol2 sources: `~/Development/Unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2/<folder>/T_<folder>_<kind>.tga`, kinds `basecolor normal roughness ambientOcclusion height`, 2048².
- Slop sources: `~/Pictures/slops/terrain/<set>/<set>_<kind>_<variant>_.png`, kinds `albedo normal roughness ao height`, 1024²; the newest variant wins.
- Output: `demo/textures/<name>/<name>_<map>.png`, RGBA8, exactly 1024², maps `albedo normal roughness ao height`.
- Material ids 0–13 keep their names, their texture paths and their material parameters.
- `MATERIAL_SLOTS` stays 32; all 32 layers are still packed.

## Review Focus

1. **A slop set whose folder name collides with a vol2 material** (`grass`). The import must land in `slop_grass`, and the existing `demo/textures/grass/` must be untouched afterwards. → Task 3.
2. **A slop set with one variant** (`check_rock` is `_00001_` only, `cracked_rock` `_00002_` only). The import must not require `_00001_`. → Task 3 (the tool already handles it; re-verified).
3. **An id between the set's end and the cap** (e.g. 20 in a 17-entry set). It must still sample entry 0, which is why the layer count stays 32. → Task 1; the existing id-40 assertion still covers the past-cap path.

---

### Task 1: 1024 in the packer and the suite

- [x] Change `tests/run_tests.gd`'s two size assertions from 256 to 1024; run the suite and watch it fail against the old library.
- [x] `TEXTURE_ARRAY_SIZE = 1024` in `voxel_materials.cpp`.
- [x] Rebuild the extension (`scons -j8 platform=macos arch=arm64 target=template_debug`) and run `godot --headless --path . -s tests/run_tests.gd`; expect `ALL GODOT TESTS PASSED`.
- [x] Commit.

### Task 2: one vol2 build script at 1024

- [x] Rewrite `demo/textures/make_textures.py`: `SIZE = 1024`, one `MAP` of material → vol2 folder for the twelve sets below, TGA input, `albedo` tinted in linear space then re-encoded, the other four maps copied as data, output to `demo/textures/<name>/`; delete `make_inner_textures.py`.

  `grass→grass_01`, `soil→mud_01`, `rock→rock`, `sand→sand_01`, `gold→stones_02` (tint `1.0,0.78,0.25`, gain `1.35`), `bedrock→cliff_stone_02`, `crystal→ice`, `turf→ground_foliage_01`, `dug_soil→ground_crack_01`, `broken_rock→ground_stone_01`, `rubble→breakstone`, `cracked_crystal→ice_crack`.
- [x] Run it; expect twelve sets × five maps at 1024² RGBA8, no missing file.
- [x] `godot --headless --import --path .` so the changed files reimport (paths and uids unchanged).
- [x] Commit.

### Task 3: slop import at 1024, three new folders

- [x] `organize_material.sh`: `size=1024`, optional third argument for the output name (default: source basename), header comment updated.
- [x] Import four sets: `brick` (refresh at 1024), `check_rock → slop_check_rock`, `cracked_rock → slop_cracked_rock`, `grass → slop_grass`.
- [x] Check `demo/textures/grass/` is unchanged (`git status` clean for that folder) and that all four new folders have five 1024² PNGs.
- [x] `godot --headless --import --path .`; commit.

### Task 4: three more materials in `materials.tres`

- [x] Add `ext_resource` entries for the twelve new slop textures (uids from their `.import` files) and three `VoxelMaterialDef` sub-resources — `Slop check rock` (id 14, `pattern_scale` 0.3, `roughness` 0.9, `hardness` 1), `Slop cracked rock` (15, same), `Slop grass` (16, `pattern_scale` 0.22, `roughness` 0.95, `hardness` 1) — and append them to `materials`.
- [x] `godot --headless --path . -s tests/run_tests.gd`; expect `ALL GODOT TESTS PASSED`.
- [x] Commit.

### Task 5: proof

- [x] `cd addons/dk_voxel && scons -j8 selftest && ./build/selftest` → `ALL CORE TESTS PASSED`.
- [x] Load `demo/main.tscn` headless: the set reports 17 materials, the bound `albedo_tex` is 32 layers at 1024², and the four arrays' byte cost is as the spec states.
- [x] Shot one frame of the demo (`--shot`) and eyeball that terrain and the new ids render.

## Ledger

- `Ruling: Brick is not duplicated` — upstream `demo-polish` already imports the slop brick as id 7 through `organize_material.sh`, so a "Slop brick" id would be the same pixels twice. Cost if wrong: the user wanted a second, independently tuned brick material; adding it later is a resource edit, not a code change.
- `Ruling: slop folders are prefixed` — the slop `grass` set would otherwise overwrite `demo/textures/grass/`, which id 1 needs. Cost if wrong: folder names differ from the source set names.
- Proof (Task 5): suite green at 1024 (`ok the arrays are packed at the common size (1024x1024)`), `ALL CORE TESTS PASSED`, set loads with 17 materials, four arrays at 32 layers of 1024² = 682 MiB with mips, first pack 1.34 s, and the demo's three `--shot` views render terrain, stone detail and the far field.
- `Ruling: Default shares Soil's pixels` — both entries point at the same texture files today, so vol2 `mud_01` is built once and referenced twice. Cost if wrong: Default cannot be tuned independently without a second import.
