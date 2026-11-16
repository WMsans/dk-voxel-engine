# dk_voxel — real textures and a designed source tree

Refactor of `addons/dk_voxel/src/`. Builds on `2026-11-14-engine-core-design.md`; nothing in it is
reopened. This document is about how the addon is *edited*, not what it does.

## Intent

The engine works: endless streaming, LOD, saves, edits and the mesher all hold. What does not hold is the
cost of changing how a world *looks* or how terrain is *shaped*.

Textures are the worst case. `VoxelMaterialDef` has no texture fields, so every surface is painted from
two colours and a seamless noise 3D texture sampled in the shader. Putting a real rock texture on rock
today means editing the resource, adding a sampler-packing path in `voxel_world.cpp`, rewriting the shader
and updating the demo — four files, in C++, before a game can see one image. The 32-material cap is written
three times and checked by nothing (`MATERIAL_SLOTS` in `voxel_world.cpp`, the `[32]` uniform arrays, and
`& 31` in the shader), so a 33rd material silently wraps onto material 1. The noise texture the shader needs
is created in `demo/main.gd`, which makes the default look part of the demo script instead of the addon.
Editing a material def in place never reaches the shader at all: only `set_material` and `set_material_set`
push uniforms.

World gen is the second. `DemoTerrain::generate_block` is one 48-line body carrying height, ridges, mesas,
overhangs, tunnels, caverns, bedrock, layering and two ores, interleaved with the voxel loop. Material ids
are bare ints (`3` rock, `6` bedrock, `5` gold) duplicated by position in the demo's `MATERIALS` array.
`hash3`, `noise3` and `fbm` are file-static, so a game's own generator has nothing to build on. The
thread-local 4-slot column cache is keyed by block *parity*, which is correct only because `fill_chunk`
happens to visit blocks in x, y, z order — a hand-written generator inherits a silent wrong-height bug.

And the tree is flat and undifferentiated. `voxel_core.h` is the directory games are told to include, and it
exposes `Volume`'s private machinery. `voxel_world.cpp` is 842 lines carrying nine jobs.

The user's decision, which shapes everything below: **textures must be addable through Godot resources with
real image files and no game code; world-gen rules are allowed to need code, but that code must be pleasant
to edit.**

## Goals

1. A material's look is a Godot resource: `VoxelMaterialDef` carries real `Texture2D` files (albedo, normal),
   assigned in the inspector. No C++, no shader edit, no rebuild, no game code.
2. No procedurally faked material textures remain. The shared noise 3D texture, the height-derivative bump
   and the demo-side material shader setup are deleted.
3. The material count is declared in two places instead of four, the two are kept honest by a test, and an
   id past the cap renders as the last slot rather than silently wrapping onto material 1. A set with more
   defs than slots is reported.
4. World gen reads as rules: named material ids, named constants, one named function per rule, noise helpers
   any generator can include, and a column cache whose correctness does not depend on the order the engine
   happens to fill blocks.
5. The source tree has seams: a Godot-free `src/core/`, and the node split by concern, with the node's
   internals out of the header games include.
6. Everything not targeted stays put. Mesher and volume behaviour, streaming, LOD, saves and the public API
   are untouched, and generated demo terrain is byte-identical to today's.

## Non-goals

- Data-driven generation settings (thresholds as resource properties). The user said rules may touch code.
- A chunk-level or heightmap `Generator` hook. The existing per-block contract stays.
- Any change to streaming, LOD selection, collision, save format or the job queue's behaviour.
- Editor viewport preview, baked-data sources, structure stamps (project 2).
- Moving or breakable voxel objects (project 3).
- Per-material texture sizes, texture atlases, parallax occlusion, triplanar tri-planar filtering improvements.
- Multiple shaders or per-material shader variants.
- A GDScript-visible material enum, or GDScript-facing generator subclasses.

## Anti-goals (a failure even if it works)

- A material set that needs a rebuild to show a texture.
- Seams between chunks or across LOD levels breaking, or ms/chunk getting worse: `selftest` and
  `tests/run_tests.gd` must pass, unchanged where the behaviour is not the target.
- Generated demo terrain changing. A change would mean the refactor was not a refactor, would invalidate
  every existing save's terrain, and would leave no way to prove the restructure was neutral.
- An abstraction introduced for its own sake — a `WorldState` that just forwards, a `ColumnCache` no
  generator uses, a file split that adds a layer without removing a tangle.
- The texture path requiring a specific texture size from the game.

## Constraints

- Godot 4.7, godot-cpp pinned in `build.sh`, C++17, Jolt physics, SCons. Linux x86_64 and macOS arm64.
- `src/core/` stays free of Godot so host-side `selftest` keeps building it.
- Voxel format unchanged: 2 bytes per voxel, optional Hermite per 8³ block, `CHUNK` 16³ cells.
- Save file header unchanged (format version, cell size, generator class name). Byte-identical terrain output
  is what makes that safe.
- `sampler2DArray` and triplanar sampling must work on both the Forward+ and Compatibility renderers.
- The addon is compiled into each game's GDExtension (`SConscript` returns the source list and the include
  directory). Renaming headers is a documented README change, not an API promise.

## Approaches considered

### How textures reach the shader

- **A. One `sampler2DArray` per map, layer = material id (chosen).** Two arrays — albedo and normal — built
  from the set, sampled with triplanar UVs from volume position. Triplanar means no UVs in the mesh and so
  no mesher change, and no stretching on cliffs or cave ceilings. Cost: the arrays need a common size and
  format, so an oversized image is resized down.
- B. `uniform sampler2D albedo[32]`, one sampler slot per material. Keeps each texture's native size, but
  binding arrays of samplers is fiddly in godot-cpp, each slot is a separate uniform to keep in sync, and
  the cap gets harder rather than easier. Rejected: it buys texture size and costs the thing we are fixing.
- C. An atlas image with grid cells. Cheapest shader, but mipmaps bleed across cell borders, the cell size is
  baked into the layout, and adding a material changes the grid and rebuilds everything. Rejected.

### How the material default works

Chosen: a def with no textures gets a generated fallback layer so colour-only materials still look like
ground. The albedo fallback is a small value-noise grain generated once in C++, the normal fallback is flat.
Rejected: shipping a `default_albedo.png` with the addon — a `res://` path from a C++ constructor is fragile
when the addon is a submodule or packed, and the failure mode is a silently missing texture. A flat white
fallback was also rejected: it makes the demo's ground visibly flatter for no gain.

### How world gen is decomposed

- **A. Decompose, keep the algorithm (chosen).** Extract the noise helpers into a header any generator can
  include, name the material ids in an enum, lift the magic numbers to a constants block, split the one
  function into one function per rule, and replace the parity-keyed column cache with one keyed on the
  coordinates it actually computed. Same operations in the same order, so the output is byte-identical and
  `selftest` can prove it.
- B. Data-driven: a settings resource with thresholds and material ids as properties. Rejected: the user
  wants rules in code, and a settings schema for terrain shape is the over-engineering this project exists
  to remove.
- C. A chunk-level `Generator` hook so generators receive a heightmap and need no cache. Rejected for now:
  it changes the engine contract that project 2's structure stamps depend on, to solve a caching problem
  that a better key fixes for free.

### How the tree is de-flattened

- **A. `src/core/` for the Godot-free half, per-concern files for the node half, internals behind a
  `unique_ptr` state (chosen).** The core directory is a boundary the build already proves: `selftest`
  compiles `volume.cpp`, `mesher.cpp` and the demo generator with no Godot. On the node side the nine jobs in
  `voxel_world.cpp` become one file each, and the node's data moves out of the public header.
- B. The same split, directory left flat. Rejected: games still include a header that shows `Volume`'s guts
  and the worker queue, and this is the complaint being answered.
- C. Three trees (`core/`, `godot/`, `demo/`). Rejected: a third directory for two files is ceremony on top
  of A.

## Design

### A. Materials and textures (Godot side)

`VoxelMaterialDef` keeps every field it has and gains two:

```
albedo: Texture2D   # tiled over the surface; tinted by color_a/color_b
normal: Texture2D   # tangent-space normal map; flat when unset
```

`color_a` / `color_b` become a tint over the albedo, driven by the albedo's own luminance so the two-tone
pattern survives without the noise texture. `pattern_scale` becomes tiles per metre of the albedo and normal.
`hardness` is unchanged. Missing textures are not an error.

New `voxel_materials.h/.cpp` owns `VoxelMaterialDef`, `VoxelMaterialSet` and the packing:

- `MATERIAL_SLOTS` (32) is one `constexpr` in this file. The shader needs a compile-time size for its
  `color_a[32]` / `params[32]` arrays and a clamp bound, and GLSL cannot read a C++ constant, so the shader
  declares its own `const int MATERIAL_SLOTS = 32;` and a `tests/run_tests.gd` check reads the
  `.gdshader` as text and asserts the two literals agree. Two declarations, one test, no codegen and no
  fixture shader: `demo/main.gd` keeps loading the shader by path.
- Packing builds two `Texture2DArray`s, layer = material id, for ids `0 .. MATERIAL_SLOTS-1`, padded with the
  fallback layers. Every layer is resized to one common square size and converted to RGBA8, mipmapped, with
  repeat enabled. The array size is one constant in this file.
- Rebuild triggers: `set_material_set`, and `changed` from the set or from any of its defs. Rebuilding
  compares image RIDs and skips the packing work when nothing about the textures changed; the
  colour/parameter uniforms are pushed unconditionally because they are cheap.
- Uniforms pushed: `albedo_tex`, `normal_tex`, and today's `color_a`, `color_b`, `params`. They are set with
  `ShaderMaterial::set_shader_parameter`, not a raw RID, so GDScript can read them back and the tests can
  inspect the packed layers. An unset or empty set packs one all-fallback material so the arrays always bind.
- Signals, so that editing materials in place works the way Godot resources normally do: every
  `VoxelMaterialDef` setter calls `emit_changed()`, `VoxelMaterialSet::set_materials` calls
  `emit_changed()`, and `VoxelMaterialSet` re-emits its defs' `changed`. The node connects to the set's
  `changed` (connecting and disconnecting as the set is replaced) and re-packs from there. `apply_hardness()`
  runs in that same path, so a hardness edit reaches the core without any additional trigger.
- Reporting: a set holding more defs than `MATERIAL_SLOTS` gets one `push_error` naming the overflow, once
  per push. A voxel id past the cap is not an error and not warned about — the voxel format allows 256 ids
  and such a voxel is legitimate data; it renders as the last slot.

The shader (`voxel_terrain.gdshader`):

- Deleted: `noise_tex`, the `bump` uniform, the screen-space height-derivative normal path, and the
  three-sample noise "surface()" function.
- Added: triplanar sampling of both arrays for each of the fragment's two material ids, UV scaled by
  `params[id].x`, weighted by `|NORMAL|` with a sharpening exponent, blended by the existing `COLOR.b`
  smoothstep.
- The albedo expression, stated exactly so it is not guessed at:
  `ALBEDO = tex.rgb * mix(color_a[id], color_b[id], luminance(tex.rgb))`. The texture carries the pattern and
  its own hue; the colours are a tint ramp. A game that wants its texture shown unmodified sets `color_a` and
  `color_b` to white, and that is what the README says. The demo's dark colours stay as they are: the fallback
  grain is near-white and its luminance drives the ramp, so colour-only materials look as they do today.
  `EMISSION` keeps scaling by the albedo and `mix(pa.w, pb.w, t)`, and `AO` keeps coming from `COLOR.a`, so
  neither reads a texture directly.
- Normals: decode both layers' tangent-space normals, blend, and apply; the flat fallback leaves the
  interpolated normal alone. A `normal_strength` uniform (default 1.0) scales the blend, since a packed
  normal can be too strong at a given tiling.
- Kept: `COLOR.r/.g` as the two ids, `COLOR.a` as baked AO, `params` for roughness/metallic/emission, rim
  light. Ids are clamped to `MATERIAL_SLOTS-1` instead of masked with `& 31`, so an out-of-range id shows
  material 31 instead of wrapping to 1.

`demo/main.gd` loses `_make_terrain_material()`'s noise setup (the shader material still loads the shader) and
gains two texture assignments, using two small PNGs committed under `demo/textures/` so the demo exercises the
real file path rather than only the fallback.

### B. World generation (core)

New `src/core/noise.h`, header-only:

```
uint32_t hash3(int x, int y, int z, uint32_t seed);
float    noise3(float x, float y, float z, uint32_t seed);   // gradient noise, roughly -1..1
float    fbm(float x, float y, float z, uint32_t seed, int octaves);
```

The bodies move out of `demo_generator.cpp` unchanged, so output stays identical.

`ColumnCache<T, N>` in the same header replaces the parity-keyed `Col`. It stores the last computed run of `N`
values together with the exact `(x, z)` origin and a caller-supplied `uint64` tag (settings hash: seed, cell
size, height). A miss recomputes; a stale entry can never be served, so a generator that calls it in any
order, or two generator instances on one thread, stays correct. `DemoTerrain` uses it for its 64 block
columns.

`demo_terrain.h/.cpp` (renamed from `demo_generator`):

- `enum DemoMaterial : uint8_t { GRASS = 1, SOIL = 2, ROCK = 3, SAND = 4, GOLD = 5, BEDROCK = 6, BRICK = 7,
  CRYSTAL = 8 }`. The demo's `MATERIALS` array order matches it; README says so.
- A constants block at the top of the .cpp holding today's numbers under names (`HEIGHT_FLOOR`, `RIDGE_*`,
  `MESA_*`, `CAVE_*`, `ORE_*`, layer depths).
- One file-local function per rule, each 5–20 lines: `column_heights` (the fbm/ridge/mesa height and its
  max), `cave_sdf`, `overhang_sdf`, `surface_material`, `ore_material`, `bedrock`.
- `generate_block` becomes composition: is the block above the height (air)? for each voxel — sample height,
  apply overhang and caves, apply the bedrock floor, pick surface/ore material, write the voxel. Same
  arithmetic, same order.

`selftest` gains two checks that make this project verifiable:

- a voxel-content hash of a fixed generated world, printed and asserted. This check lands *first*, on the
  untouched tree, and its value is recorded in the plan before any world-gen code moves — it is the only
  thing that proves the later restructure was neutral, so it cannot be added alongside the change it is
  meant to audit;
- a scan of that world's material ids, asserting every id is a `DemoMaterial` value (catches a new ore using
  an unlisted id, and the demo/GDScript drift the bare ints allowed).

### C. Source layout

```
src/
  core/
    voxel_types.h        Vec3, Voxel, Block, BLOCK/CHUNK/UNBREAKABLE, to_u8, sdf_to_density, floordiv,
                         ChunkKey + hash, MeshData, Shape, Mode, EditResult, RayHit
    voxel_generator.h    struct Generator
    voxel_volume.h       class Volume            voxel_volume.cpp   (from volume.cpp, unchanged)
    voxel_mesher.cpp     dual contouring          (from mesher.cpp, unchanged)
    noise.h              hash3 / noise3 / fbm / ColumnCache
  demo_terrain.h/.cpp    DemoTerrain, DemoMaterial, the named rules
  voxel_world.h          the node's public API only
  voxel_world_state.h    struct WorldState: volume, chunk table, viewer list, job queue, results,
                         save directory + memory store, material set, stats
  voxel_world.cpp        node lifecycle, properties, edit entry points, queries, bindings, _process loop
  voxel_stream.cpp       update_viewers, scan, stream_endless, worker(), stop/start, job queue
  voxel_chunks.cpp       chunk RID lifecycle: mesh surface upload, collision body, free_chunk
  voxel_save.cpp         chunk file read/write, the compressed memory store, save(), reset()'s flush
  voxel_materials.h/.cpp VoxelMaterialDef / VoxelMaterialSet / texture arrays / uniform push
  voxel_resources.h/.cpp VoxelGenerator resource, VoxelViewer
  register_types.*, entry.cpp
```

`VoxelWorld` holds `std::unique_ptr<dkv::WorldState>`, so the queue, the chunk table and the volume leave the
public header. Bound method names, properties and the `terrain_edited` signal are unchanged. The files above
take `WorldState &` (and the node where Godot calls are needed) as parameters; nothing gets a virtual
interface, and no behaviour changes hands.

`SConscript`'s source list and the `selftest` program's source list are updated for the new paths. A game's
`SConstruct` line is unchanged because `SConscript` still returns the list and the include directory; games
now write `#include "core/voxel_generator.h"`.

`README.md` is updated for: the two texture fields and that they take real image files, the material id enum,
the new header names, and the file map above.

## What's left to the builder

- The common texture array size and the fallback grain's noise parameters.
- The exact content of the two demo PNGs, and which two demo materials wear them.
- `ColumnCache`'s template shape, and whether the rule functions are free functions or one struct.
- How `WorldState`'s fields are divided between the files above; whether `VoxelViewer` gets its own file.
- Whether rebuilding the arrays on every `changed` needs a cheap "textures unchanged" fingerprint memo.
- The order of the refactor steps, with one exception that is fixed above: the content hash is captured
  first. Otherwise each step need only leave `selftest` and `tests/run_tests.gd` green.

## Testing

- `selftest` (no Godot, host toolchain):
  - every existing check, unchanged in intent: seams bit-exact for any LOD mix, generation deterministic,
    serialise round-trip, edits clipped to loaded chunks, concurrent fill/query;
  - the generated-world content hash, equal to the pre-refactor value;
  - every material id in a generated world is a `DemoMaterial` value;
  - `ColumnCache` returns correct heights when called out of order and after a settings change.
- `tests/run_tests.gd` (Godot, headless):
  - every existing check, unchanged;
  - a set whose defs carry `Texture2D`s packs arrays with one layer per slot, read back from the
    `albedo_tex` / `normal_tex` shader parameters and inspected with `Texture2DArray::get_layer_data`. The
    textures are built at runtime from an `Image` (an `ImageTexture`), so no import step and no fixture files
    are needed headless; the demo's committed PNGs exist for the demo, not for the tests;
  - editing a def's texture in place after `set_material_set` re-packs and reaches the shader, and replacing
    `VoxelMaterialSet.materials` does too;
  - the `.gdshader` text declares the same `MATERIAL_SLOTS` the C++ does;
  - a voxel id past `MATERIAL_SLOTS` is written, meshed and rendered without error, and does not wrap: the
    colour it reports is the last slot's;
  - a world with no material set at all still packs a bindable default.
- Rendered check, both renderers: the demo's existing `--shot` path, run once with the default Forward+
  driver and once with `--rendering-driver opengl3`, confirming the shader compiles, the arrays bind and the
  two textured materials show their texture. Manual, no new CI plumbing: the headless suite never compiles a
  shader, so this is the only thing that proves the `sampler2DArray` and triplanar path at all.

## Build note

`godot-cpp` is not checked in; `build.sh` clones it. An existing clone sits at
`.worktrees/engine-core-work/addons/dk_voxel/thirdparty/godot-cpp` and can be copied to
`addons/dk_voxel/thirdparty/` to avoid the clone.
