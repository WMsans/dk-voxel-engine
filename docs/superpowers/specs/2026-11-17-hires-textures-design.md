# dk_voxel — 1024 textures, one pack, and slop materials

Builds on `2026-11-16-textures-and-modularity-design.md`; the packing path, the shader and the resource
contract are not reopened. This only changes what the layers are made of and how big they are.

## Intent

Two things. The demo's terrain looks soft at close range because every layer of the four
`Texture2DArray`s is 256 px. And there are two texture sources for one job: the outer materials come from the
*voxel-everything* pack, the broken insides from `terrain_textures_vol2` — both run through
`demo/textures/make_*.py`, each carrying its own copy of the same 256 px pipeline.

## Goal

1. `TEXTURE_ARRAY_SIZE` is 1024, not 256, and every material is built from `terrain_textures_vol2` at that
   size, except where the slop pack is already the source.
2. One script builds every vol2-sourced texture (`make_textures.py` absorbs `make_inner_textures.py`);
   `tools/organize_material.sh` keeps importing the slop pack, at 1024.
3. The generated slop set (`~/Pictures/slops/terrain`) is available as three more materials, ids 14–16,
   alongside the existing fourteen. Nothing in world gen uses them yet; they are there to be assigned.

## Decisions

- **Size.** 1024 is a downscale of vol2's 2048² sources and a straight copy of the slop pack's 1024². The
  `pattern_scale` values (tiles per metre) are unchanged, so the tiling is identical and only sharpness
  changes.
- **Memory, accepted by the user.** 4 arrays × 32 layers × 1024² RGBA8 ≈ 512 MiB VRAM, ~683 MiB with
  mipmaps (32 MiB today), plus a second or two of extra pack time on the main thread at first bind. Layer
  count stays 32 and nothing is compressed: packing fewer layers would make an id past the set's end sample
  nothing, where today it samples entry 0.
- **Brick is already the slop brick** (branch `demo-polish`, commit `30bd3c3`, imported through
  `organize_material.sh`). It is re-imported at 1024 and no duplicate "Slop brick" material is added.
- **Mapping**, ids and names unchanged, so world gen keeps working untouched:

  | id | material | source |
  |----|----------|--------|
  | 0 | Default | vol2 `mud_01`, the same files as Soil |
  | 1 | Grass | vol2 `grass_01` |
  | 2 | Soil | vol2 `mud_01` |
  | 3 | Rock | vol2 `rock` |
  | 4 | Sand | vol2 `sand_01` |
  | 5 | Gold | vol2 `stones_02`, gold tint and gain kept |
  | 6 | Bedrock | vol2 `cliff_stone_02` |
  | 7 | Brick | slops `brick`, newest variant (already) |
  | 8 | Crystal | vol2 `ice` |
  | 9 | Torn turf | vol2 `ground_foliage_01` |
  | 10 | Dug soil | vol2 `ground_crack_01` |
  | 11 | Broken rock | vol2 `ground_stone_01` |
  | 12 | Rubble | vol2 `breakstone` |
  | 13 | Cracked crystal | vol2 `ice_crack` |
  | 14 | Slop check rock | slops `check_rock` |
  | 15 | Slop cracked rock | slops `cracked_rock` |
  | 16 | Slop grass | slops `grass` |

- **Slop variant** is the newest numbered one, which is what `organize_material.sh` already picks — so
  `brick` and `grass` take `_00002_`, `check_rock` its only `_00001_`.
- **Slop folders** are prefixed `slop_` for the three new materials (`slop_check_rock`, `slop_cracked_rock`,
  `slop_grass`) so that `slop_grass` does not clobber the vol2-built `demo/textures/grass/`. The tool gains
  an optional name argument for this; `brick` keeps its folder.
- **Maps per material:** albedo, normal, roughness, ao, height — the five the two packs both carry and the
  five the repo already writes. The engine still ignores height.
- **Gold keeps its baked tint** (`1.0, 0.78, 0.25` × 1.35 in linear space) — the one material whose colour
  is not its texture's.
- **New slop materials carry no `inner_material`** (`-1`) and `hardness = 1`, so a game that skips wiring
  them up cannot make them dig into something unintended.

## Non-goals

- Per-material texture sizes, atlases, VRAM compression, streaming the arrays, raising `MATERIAL_SLOTS`.
- Using the new slop materials in world gen, or reassigning any existing id.
- Touching the shader, the packing code beyond one constant, or making the engine sample height.

## Constraints

- `TEXTURE_ARRAY_SIZE` is written in the C++ packer and mirrored by the two texture tools; the Godot suite
  asserts the packed size.
- Normal maps stay OpenGL (Y+), which both packs already are.
- `demo/textures/<name>/<name>_<map>.png` stay RGBA8 PNG at the array size, so packing is a straight copy.
- The fourteen existing material names, ids and texture paths do not change, so `materials.tres` is
  re-pointed rather than redesigned.
