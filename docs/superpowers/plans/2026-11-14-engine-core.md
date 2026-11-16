# dk_voxel Engine Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the fixed-box `VoxelWorld` demo into a reusable engine core: sparse streaming chunk storage (endless or bounded), pluggable C++ generators, `VoxelViewer` nodes, saving of edited chunks, change events, and material-set resources.

**Architecture:** The Godot-free core (`dkv::Volume`) swaps its flat block array for a hash map of 16³-cell chunks. It gains a `Generator` interface, chunk serialisation, and chunk-relative mesh output. The Godot node (`VoxelWorld`) keeps its edit/query API, replaces `create()`/`generate_terrain()` with properties, and schedules generate/mesh jobs around every `VoxelViewer`. Games compile the addon's sources into their own GDExtension.

**Tech Stack:** C++17, godot-cpp (pinned in `build.sh`), Godot 4.7.2, Jolt, SCons. Host-built `selftest` for the core; `godot --headless -s tests/run_tests.gd` for engine tests.

**Spec:** `docs/superpowers/specs/2026-11-14-engine-core-design.md`

## Global Constraints

- `voxel_core` (everything under `src/` except `voxel_world.cpp` and other files that include godot-cpp) has no Godot dependency; `scons selftest` builds it with the host toolchain.
- Voxel format unchanged: 2 bytes (density u8, iso 127.5; material u8), optional Hermite data 512 × 9 bytes per 8³ block.
- `CHUNK = 16` cells, `BLOCK = 8`. The mesher reads samples −2..17 around a chunk, so it needs the chunk and all 26 neighbours.
- Seams stay bit-exact for any LOD mix (`open_edges == 0` in every `mesh_all` check).
- Mesh speed must not regress. Baseline on this machine: terrain `0.66 ms/chunk` LOD0, `0.43 ms/chunk` LOD2, `generate 63 ms` for 8×4×8 chunks (`./build.sh --test`). Stay within +10%.
- Edits are main-thread calls; generation and meshing run on worker threads; one global `shared_mutex` (`// ponytail: one global lock; per-chunk locks if workers contend.`).
- Works within about 8 km of the origin. Mesh and collision vertices are relative to the chunk origin.
- No editor streaming: in the editor (`Engine::is_editor_hint()`) no workers start and nothing is meshed.
- No compatibility shims: `create()` and `generate_terrain()` are removed; demo and tests move to the new API.
- Verification commands (from `addons/dk_voxel/`): `./build.sh --test` must print `ALL CORE TESTS PASSED`. From the repo root: `godot --headless --path . -s tests/run_tests.gd` must print `ALL GODOT TESTS PASSED` and exit 0.

## Review Focus

1. **Negative coordinates in endless worlds.** `>> 3`, `& 7` and chunk division on negative voxel coordinates must keep the mesh closed across the origin. Test: `endless sphere across the origin` (Task 1).
2. **Edits that straddle loaded and unloaded chunks.** They must apply to the loaded part, report only what was applied, and never create chunks. Test: `edit clips at unloaded chunks` (Task 3).
3. **Teleporting a viewer** (respawn, fast travel). Old chunks must unload, with meshes, bodies and RIDs freed, and no leak. Test: `teleport frees far chunks` (Task 5).
4. **`reset()` while workers are mid-job** (the demo's R key pressed repeatedly). No crash, no stale results applied to the new world. Test: `reset while busy` (Task 5).
5. **Freeing a world without calling `save()`.** Modified chunks must still reach disk. Test: `free without save keeps edits` (Task 6).

---

## File structure

| File | Responsibility |
|---|---|
| `addons/dk_voxel/src/voxel_core.h` | Core types (`Vec3`, `Voxel`, `Block`, `MeshData`, `EditResult`, `RayHit`), `Generator`, `Volume` declaration. |
| `addons/dk_voxel/src/volume.cpp` (new, from `voxel_core.cpp`) | Chunk map, fill, edits, queries, serialisation. |
| `addons/dk_voxel/src/mesher.cpp` (new, from `voxel_core.cpp`) | `Volume::mesh_chunk` and its QEF/material helpers, unchanged algorithm. |
| `addons/dk_voxel/src/demo_generator.h/.cpp` (new) | `dkv::DemoTerrain`, today's hills/caves/ore as a `Generator`. |
| `addons/dk_voxel/src/selftest.cpp` | Core checks. |
| `addons/dk_voxel/src/voxel_world.h/.cpp` | `VoxelWorld` node: properties, API, signals, scheduling, upload. |
| `addons/dk_voxel/src/voxel_resources.h/.cpp` (new) | `VoxelGenerator`, `DemoTerrainGenerator`, `VoxelMaterialDef`, `VoxelMaterialSet`, `VoxelViewer`. |
| `addons/dk_voxel/src/register_types.h/.cpp` (new) | `dk_voxel_register_types()` (shared) and the addon's own entry symbol. |
| `addons/dk_voxel/SConscript` (new) | Source list and include path that both the addon's SConstruct and a game's SConstruct use. |
| `addons/dk_voxel/voxel_terrain.gdshader` | 32 materials. |
| `demo/main.gd`, `tests/run_tests.gd`, `addons/dk_voxel/README.md` | Ported to the new API. |

`voxel_core.cpp` is deleted once split.

---

### Task 1: Sparse chunk storage and chunk-relative meshes

**Files:**
- Modify: `addons/dk_voxel/src/voxel_core.h`
- Create: `addons/dk_voxel/src/volume.cpp`, `addons/dk_voxel/src/mesher.cpp` (split from `voxel_core.cpp`, then delete it)
- Modify: `addons/dk_voxel/SConstruct` (source lists), `addons/dk_voxel/src/voxel_world.cpp` (adapt to new `Volume`)
- Test: `addons/dk_voxel/src/selftest.cpp`

**Interfaces:**
- Produces (in `voxel_core.h`, namespace `dkv`):
  ```cpp
  struct ChunkKey { int x, y, z; bool operator==(const ChunkKey &) const; };
  struct ChunkKeyHash { size_t operator()(const ChunkKey &) const; };
  inline int floordiv(int a, int b); // moved to the header, used by core and node
  class Volume {
  public:
      explicit Volume(float cell_size);                          // endless, starts empty
      Volume(int chunks_x, int chunks_y, int chunks_z, float cell_size); // bounded [0, n), all chunks present as air
      const float cell;
      const bool bounded;
      const int bmin[3], bmax[3];                                 // chunk bounds, exclusive max; unused if !bounded
      float lod_error[3] = {0.0f, 0.02f, 0.12f};
      uint8_t hardness[256] = {};
      bool in_bounds(int cx, int cy, int cz) const;               // always true when endless
      bool has_chunk(int cx, int cy, int cz) const;
      bool neighbors_ready(int cx, int cy, int cz) const;         // chunk + 26 neighbours present, or outside bounds
      std::vector<ChunkKey> loaded_chunks() const;
      size_t chunk_count() const;
      EditResult edit(Shape, Mode, Vec3 center, Vec3 half, const float basis[9], uint8_t material, int power, float strength, bool sharp);
      bool mesh_chunk(int cx, int cy, int cz, int lod, MeshData &out) const; // positions relative to chunk origin
      Vec3 chunk_origin(int cx, int cy, int cz) const;            // metres: (c * CHUNK) * cell
      RayHit raycast(Vec3 origin, Vec3 dir, float max_dist) const;
      float density(Vec3 p) const;
      Voxel get(int x, int y, int z) const;                       // missing chunk or outside bounds = air
  };
  ```
- `EditResult.cmin/cmax` stay inclusive chunk ranges. Clamp to bounds when bounded, unclamped when endless.
- Remove `cx, cy, cz, nx, ny, nz` members. Bounded callers use `bmax`.

- [ ] **Step 1: Update `mesh_all` in selftest for relative positions and arbitrary chunk ranges**

`mesh_all(const Volume &v, lod, int lo[3] = bmin, int hi[3] = bmax)` adds `v.chunk_origin(x, y, z)` to every vertex before the edge and volume checks. Existing checks stay as they are.

- [ ] **Step 2: Add the failing test `endless sphere across the origin`**

```cpp
printf("endless sphere across the origin\n");
{
    Volume v(0.5f);
    for (int z = -2; z < 2; z++) for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) v.fill_chunk(x, y, z, nullptr); // added in Task 2; until then use a test-only air fill
    v.edit(SPHERE, BUILD, {0.3f, -0.2f, 0.1f}, {7, 0, 0}, ID, 3, 0, 1, false);
    double want = 4.0 / 3.0 * PI * 343;
    Soup a = mesh_all(v, lod0, {-2, -2, -2}, {2, 2, 2}), b = mesh_all(v, mixed, {-2, -2, -2}, {2, 2, 2});
    CHECK(a.open_edges == 0 && b.open_edges == 0, "open edges %ld %ld", a.open_edges, b.open_edges);
    CHECK(std::fabs(a.volume - want) < want * 0.01, "volume %.1f", a.volume);
}
```

To get chunks present before Task 2 exists, Task 1 adds `void fill_chunk(int cx, int cy, int cz, const Generator *g)` with only the `g == nullptr` (air) path, and declares `struct Generator;` forward. Task 2 completes it.

- [ ] **Step 3: Run `./build.sh --test`. Expected: compile failure (new constructor and `fill_chunk` missing).**

- [ ] **Step 4: Implement the chunk map in `volume.cpp`**

- `struct Chunk { std::unique_ptr<Block> b[8]; Voxel uniform[8]; bool modified = false; };`, stored in `std::unordered_map<ChunkKey, std::unique_ptr<Chunk>, ChunkKeyHash>`.
- Block index inside a chunk: `((bz & 1) << 2) | ((by & 1) << 1) | (bx & 1)`. Voxel index inside a block: `vidx` as today (`& 7` works for negatives in two's complement). Chunk from voxel: `x >> 4` (arithmetic shift, correct for negatives).
- Bounded `get()` keeps today's closed-world rule. Valid voxels are `bmin*16 + 1 .. bmax*16 - 1` on each axis; everything else reads as air.
- `writable()` returns `nullptr` for a missing chunk (it never creates one) and sets `modified`.

- [ ] **Step 5: Move `mesh_chunk` to `mesher.cpp` with only these changes**

- **Gather:** resolve the 27 chunk pointers once (or per block) under the shared lock. Don't call `get()` per voxel through the hash map. The 20³ sample copy must stay a plain array walk.
- **Reject test:** the cheap reject over blocks uses the same lookup.
- **Output:** `out.pos` is `(v.o + v.f) * cell`, relative to the chunk origin. Drop the `ox/oy/oz` term.

- [ ] **Step 6: Adapt `voxel_world.cpp` so the game still runs**

- Chunk index `i` → `ChunkKey` via bounds.
- Each chunk instance and body transform is `gx() * Transform3D(Basis(), chunk_origin)`. Update it in `_notification(TRANSFORM_CHANGED)` too.
- `create()` constructs the bounded `Volume`. This is a temporary bridge; Task 4 replaces it.

- [ ] **Step 7: Run `./build.sh --test`. Expected: `ALL CORE TESTS PASSED`; terrain ms/chunk within +10% of 0.66 / 0.43.**

- [ ] **Step 8: Run `godot --headless --path ../.. -s tests/run_tests.gd`. Expected: `ALL GODOT TESTS PASSED`.**

- [ ] **Step 9: Commit**

```bash
git add -A src SConstruct && git commit -m "refactor(core): sparse chunk map, chunk-relative meshes"
```

---

### Task 2: Generator interface and the demo terrain generator

**Files:**
- Modify: `addons/dk_voxel/src/voxel_core.h`, `addons/dk_voxel/src/volume.cpp`
- Create: `addons/dk_voxel/src/demo_generator.h`, `addons/dk_voxel/src/demo_generator.cpp`
- Modify: `addons/dk_voxel/src/voxel_world.cpp` (`generate_terrain` uses `DemoTerrain` via `fill_all` until Task 4)
- Test: `addons/dk_voxel/src/selftest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace dkv {
  struct Generator {
      virtual ~Generator() = default;
      virtual void generate_block(const int origin[3], float cell, Voxel out[512]) const = 0; // index = vidx
  };
  // Volume:
  void fill_chunk(int cx, int cy, int cz, const Generator *g); // g == nullptr = air. Thread-safe: generates without the lock, inserts under a unique lock. Replaces an existing chunk.
  void fill_all(const Generator *g, int threads);             // bounded only: every chunk in bounds
  // demo_generator.h
  struct DemoTerrain : Generator {
      uint32_t seed = 7;
      float height = 64.0f; // metres; plays the role of today's H = ny * cell
      void generate_block(const int origin[3], float cell, Voxel out[512]) const override;
  };
  }
  ```
- `Volume::generate()` is removed.

- [ ] **Step 1: Write the failing tests**

```cpp
printf("generator\n");
{
    DemoTerrain g; g.seed = 1234; g.height = 32.0f;            // = 4 chunks * 16 * 0.5, today's terrain test
    Volume v(8, 4, 8, 0.5f);
    auto t0 = std::chrono::steady_clock::now();
    v.fill_all(&g, 8);
    double gen = ms_since(t0);
    // existing terrain checks move here unchanged (tris > 10000, open edges, flipped, lod2 volume)
    CHECK(gen < 63 * 1.1 * 1.5, "generation slower than baseline: %.0f ms", gen); // generous for noise; print it
    Volume e(0.5f);
    e.fill_chunk(3, 1, -5, &g); Volume f(0.5f); f.fill_chunk(3, 1, -5, &g);
    bool same = true;
    for (int z = 0; z < 16; z++) for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
        same &= e.get(48 + x, 16 + y, -80 + z) == f.get(48 + x, 16 + y, -80 + z);
    CHECK(same, "generation not deterministic");
}
```

- [ ] **Step 2: Run `./build.sh --test`. Expected: compile failure.**

- [ ] **Step 3: Implement `DemoTerrain::generate_block`**

Move the per-voxel body of today's `Volume::generate()` into it. World coordinates come from `origin + local` times `cell`. `H` becomes `height`. If generation time goes over +10%, the 64 per-column heights are the hot part: add a `thread_local` cache keyed by `(seed, column x, column z, cell)`. That decision is the builder's (spec, "Left to the builder").

- [ ] **Step 4: Implement `fill_chunk` and `fill_all`**

`fill_chunk` calls `generate_block` for the 8 blocks and collapses uniform blocks with today's rule (`same density and (same material or air)`). `fill_all` splits the chunk list across `threads` with an atomic counter, as `generate()` does today.

- [ ] **Step 5: Run `./build.sh --test`. Expected: `ALL CORE TESTS PASSED`.**

- [ ] **Step 6: Commit** — `git commit -m "feat(core): Generator interface, DemoTerrain"`

---

### Task 3: Chunk serialisation, unload, and edit clipping

**Files:**
- Modify: `addons/dk_voxel/src/voxel_core.h`, `addons/dk_voxel/src/volume.cpp`
- Test: `addons/dk_voxel/src/selftest.cpp`

**Interfaces:**
- Produces (Volume):
  ```cpp
  std::vector<uint8_t> serialize_chunk(int cx, int cy, int cz) const; // empty if missing
  bool deserialize_chunk(int cx, int cy, int cz, const uint8_t *data, size_t size); // false on bad data; replaces chunk
  void remove_chunk(int cx, int cy, int cz);
  bool is_modified(int cx, int cy, int cz) const;
  void clear_modified(int cx, int cy, int cz);
  ```
- Byte layout (uncompressed; the node compresses): `u8 format = 1`, then per block (8): `u8 kind` (0 uniform, 1 dense, 2 dense + Hermite), then the uniform voxel (2 bytes), or 1024 voxel bytes, or 1024 + 4608. Deserialised chunks are not `modified`.

- [ ] **Step 1: Write the failing tests**

```cpp
printf("serialise\n");
{
    Volume v(0.5f);
    for (int z = -1; z <= 1; z++) for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) v.fill_chunk(x, y, z, nullptr);
    v.edit(BOX, BUILD, {1.1f, 2.2f, 0.3f}, {3, 2, 2.5f}, ID, 7, 0, 1, true); // sharp: Hermite present
    CHECK(v.is_modified(0, 0, 0), "edit did not mark the chunk");
    MeshData before; v.mesh_chunk(0, 0, 0, 0, before);
    auto bytes = v.serialize_chunk(0, 0, 0);
    v.remove_chunk(0, 0, 0);
    CHECK(!v.has_chunk(0, 0, 0) && !v.neighbors_ready(0, 0, 0), "remove");
    CHECK(v.deserialize_chunk(0, 0, 0, bytes.data(), bytes.size()) && !v.is_modified(0, 0, 0), "restore");
    MeshData after; v.mesh_chunk(0, 0, 0, 0, after);
    CHECK(before.pos == after.pos && before.idx == after.idx, "round trip changed the mesh");
    CHECK(!v.deserialize_chunk(0, 0, 0, bytes.data(), bytes.size() - 1), "truncated data accepted");
}
printf("edit clips at unloaded chunks\n");
{
    Volume v(0.5f);
    v.fill_chunk(0, 0, 0, nullptr);                                   // only one chunk loaded
    EditResult r = v.edit(SPHERE, BUILD, {8, 4, 4}, {6, 0, 0}, ID, 3, 0, 1, false); // reaches into x = 1
    CHECK(r.changed && v.chunk_count() == 1 && !v.has_chunk(1, 0, 0), "edit created a chunk");
    EditResult e = v.edit(SPHERE, ERASE, {8, 4, 4}, {6, 0, 0}, ID, 0, 0, 1, false);
    double m3 = 0; for (float f : e.removed) m3 += f * 0.125;  // removed is in voxels; 0.5^3 m3 each
    CHECK(m3 > 50 && m3 < 0.5 * 4.0 / 3.0 * 3.14159 * 216, "removed %.0f m3: should be only the part inside chunk 0 (< half sphere)", m3);
}
```

- [ ] **Step 2: Run `./build.sh --test`. Expected: compile failure.**
- [ ] **Step 3: Implement the five methods. `edit()` skips voxels whose chunk is missing (`writable()` already returns `nullptr`).**
- [ ] **Step 4: Run `./build.sh --test`. Expected: `ALL CORE TESTS PASSED`.**
- [ ] **Step 5: Commit** — `git commit -m "feat(core): chunk serialisation, unload, clipped edits"`

---

### Task 4: Godot resources, properties instead of `create()`, materials, bounded worlds

**Files:**
- Create: `addons/dk_voxel/src/voxel_resources.h/.cpp`, `addons/dk_voxel/src/register_types.h/.cpp`, `addons/dk_voxel/src/voxel_world.h`
- Modify: `addons/dk_voxel/src/voxel_world.cpp` (move the class declaration to the header; drop the entry point), `addons/dk_voxel/voxel_terrain.gdshader`, `addons/dk_voxel/SConstruct`
- Modify: `demo/main.gd`, `tests/run_tests.gd`

**Interfaces:**
- Produces (Godot classes):
  - `VoxelGenerator : Resource` (abstract), with C++ `virtual std::shared_ptr<const dkv::Generator> create_generator() const = 0;` Games subclass it in C++. The world calls it on start and on `reset()`, so property changes apply on the next `reset()`.
  - `DemoTerrainGenerator : VoxelGenerator`, properties `seed: int = 7` and `height: float = 64.0`.
  - `VoxelMaterialDef : Resource`, properties `name: String`, `color_a: Color`, `color_b: Color`, `pattern_scale: float = 0.3`, `roughness: float = 0.9`, `metallic: float = 0.0`, `emission: float = 0.0`, `hardness: int = 0` (0..255, 255 = unbreakable).
  - `VoxelMaterialSet : Resource`, property `materials: Array[VoxelMaterialDef]` (index = material id).
  - `VoxelWorld` properties: `cell_size: float = 0.5`, `bounds_size: Vector3i = (0,0,0)` (chunks from the origin; zero = endless), `generator: VoxelGenerator`, `material_set: VoxelMaterialSet`, `threads: int = 0` (0 = cores − 2, min 1), plus the existing `material`, `lod1_distance`, `lod2_distance`, `collision_distance`, `collision_layer`. Method `reset()`. Removed: `create`, `generate_terrain`, `set_material_hardness`. (Deviation from the spec: `bounds: AABB` becomes `bounds_size: Vector3i`. Chunk bounds are integers, and this avoids float rounding. Bounds start at chunk 0.)
  - `register_types.h`: `void dk_voxel_register_types();` registers every addon class. `register_types.cpp` defines the addon's own `dk_voxel_library_init` entry symbol, which calls it. Games don't compile that file.
- Bounded lifecycle: `ensure_volume()` runs on the first edit, query or `_ready`. It builds `Volume(bounds_size…, cell_size)`, applies material-set hardness, and calls `fill_all(generator)` synchronously (air if no generator). Workers start in `_ready` (not in the editor). Endless worlds are built empty by `ensure_volume()` (streaming arrives in Task 5).
- Material set → shader: when `material` is a `ShaderMaterial`, the world sets uniforms `color_a`, `color_b` (`PackedColorArray`, 32) and `params` (`Array` of 32 `Vector4(pattern_scale, roughness, metallic, emission)`). Missing ids use entry 0. The shader's arrays become `[32]`, and the id mask becomes `& 31`.

- [ ] **Step 1: Port `tests/run_tests.gd` to the new API (these are the failing tests)**

- `w.bounds_size = Vector3i(6, 4, 6)`, `w.cell_size = 0.5`, then `root.add_child(w)`. Remove `create()`.
- `set_material_hardness(3, 255)` becomes a `VoxelMaterialSet` with `materials[3].hardness = 255` assigned before the final hardness check.
- Add `check(VoxelWorld.new().get_world_size() == Vector3.ZERO, "endless world reports zero size")`.

- [ ] **Step 2: Run the Godot tests. Expected: FAIL (unknown property `bounds_size`).**
- [ ] **Step 3: Implement the classes, the lifecycle and the shader change above.**
- [ ] **Step 4: Port `demo/main.gd`**

- The `MATERIALS` dictionary becomes a `VoxelMaterialSet` built in `_ready` with the same values. `_make_terrain_material()` no longer sets the colour arrays.
- The world is set up as `bounds_size = Vector3i(32, 8, 32)` with a `DemoTerrainGenerator` (`height = 64`).
- R sets `generator.seed = randi()` then calls `reset()`.
- The fragment and debris colours read the set.

- [ ] **Step 5: Run `./build.sh --test` and the Godot tests. Expected: both pass. Then run `godot --path ../.. -- --shot=/tmp/shot.png` and look at the three PNGs. Expected: same look as before (same materials, ruins, terrain).**
- [ ] **Step 6: Commit** — `git commit -m "feat: generator/material resources, property-driven VoxelWorld"`

---

### Task 5: Viewers, endless streaming, events

**Files:**
- Modify: `addons/dk_voxel/src/voxel_resources.h/.cpp` (`VoxelViewer`), `addons/dk_voxel/src/voxel_world.h/.cpp`
- Test: `tests/run_tests.gd`

**Interfaces:**
- Produces:
  - `VoxelViewer : Node3D`, properties `view_distance: float = 160`, `vertical_view_distance: float = 96`, `collision_distance: float = 40`, `enabled: bool = true`. On enter tree it joins group `dk_voxel_viewers`. A world uses the viewers in that group whose `get_world_3d()` matches its own.
  - Camera fallback (no enabled viewers): the active camera with `view_distance = lod2_distance + CHUNK*cell`, `vertical_view_distance` the same, and `collision_distance` = the world's.
  - `VoxelWorld` signal `terrain_edited(aabb: AABB, result: Dictionary)`. It's emitted by every edit call with `result.changed == true`, and `aabb` is the shape's global bounds.
  - `VoxelWorld.is_area_ready(aabb: AABB, need_collision: bool = true) -> bool`: every chunk overlapping `aabb` (global) is loaded and meshed at its wanted LOD (empty chunks count as meshed), and, if `need_collision`, has a body or has no surface.
  - `get_stats()` gains `chunks` (loaded count).
- Streaming (endless only; bounded worlds keep every chunk and use viewers only for LOD and collision):
  - **Scan trigger:** any viewer changes chunk, or after an edit.
  - **Wanted set:** chunks within any viewer's horizontal `view_distance` (cylinder) and `vertical_view_distance`, plus a 1-chunk margin.
  - **Load:**
    - wanted chunks that aren't loaded become *generate jobs* on the worker queue;
    - a worker restores from the in-memory store if present, otherwise calls `fill_chunk(generator)`;
    - priority is distance to the nearest viewer.
  - **Mesh:** only when `neighbors_ready`, using the existing job logic. Meshing excludes the margin ring.
  - **Unload:** loaded chunks farther than every viewer's distance + one chunk:
    - free their RIDs;
    - if `is_modified`, `serialize_chunk` into `std::unordered_map<ChunkKey, std::vector<uint8_t>> store` (compressed with `Marshalls`/`PackedByteArray::compress(ZSTD)` on the main thread);
    - then `remove_chunk`.
  - **LOD and collision:** LOD from the nearest viewer's distance (today's bands and hysteresis). Collision if within any viewer's `collision_distance` (viewer value wins).
  - **`reset()`:** stops workers, discards queued and finished results, frees all RIDs, clears `store`, and drops the volume. Results from before the reset must never be applied: a generation counter is stored in each job and checked in `apply`.

- [ ] **Step 1: Add the failing Godot tests to `tests/run_tests.gd`**

- **`second viewer gets collision`:** bounded `Vector3i(24, 4, 6)` world, slab `build_box(Transform3D(Basis(), Vector3(96, 6, 24)), Vector3(94, 4, 20), 3)`. Camera at `(10, 14, 24)`. A `VoxelViewer` at `(180, 14, 24)` with `collision_distance = 30`. A ball at `(180, 12, 24)` must rest at `y = 10.5 ± 0.05` after 150 physics frames.
- **`endless crater survives unload`:**
  - endless world with `DemoTerrainGenerator(seed = 3)`, no camera, one viewer at `(0, 40, 0)`;
  - wait for `is_area_ready(AABB(Vector3(-4, 0, -4), Vector3(8, 64, 8)))`;
  - `g = raycast((0, 63, 0), DOWN, 80)`, then `dig_sphere(g.position, 3, 255)`;
  - move the viewer to `(600, 40, 0)` and wait until `raycast` at x = 0 returns empty (unloaded);
  - move back, wait ready, and expect the new ground `y < g.position.y - 2.5`.
- **`teleport frees far chunks`:** after the move to x = 600 and loading there, `stats.meshes` and `stats.bodies` are no greater than right after the first load at x = 0, plus 20%.
- **`reset while busy`:** call `reset()` on two consecutive frames while `get_pending_count() > 0`, then wait ready. No errors are printed, and `is_area_ready` becomes true.
- **`terrain_edited fires`:** connect the signal, `dig_sphere`, and check it fired once, with `aabb` containing the dig centre and `result.changed`.
- The existing `spawned` / readiness waits in the test switch to `is_area_ready`.

- [ ] **Step 2: Run the Godot tests. Expected: FAIL on the new checks.**
- [ ] **Step 3: Implement `VoxelViewer`, the streaming above, `is_area_ready`, and the signal.**
- [ ] **Step 4: Switch the demo to an endless world**

- `bounds_size = Vector3i.ZERO`, and the player gets a `VoxelViewer`.
- The spawn waits on `is_area_ready` around the spawn point.
- `_build_ruins` and `_beauty_shot` run after the spawn area is ready.

- [ ] **Step 5: Run the core and Godot tests (both pass). Then play the demo: sprint and fly (V) in one direction for 60 s. Expected: no holes within `collision_distance`, `chunks` in the HUD stays bounded, ms/chunk similar to before.**
- [ ] **Step 6: Commit** — `git commit -m "feat: VoxelViewer, endless streaming, terrain_edited, is_area_ready"`

---

### Task 6: Saving to disk

**Files:**
- Modify: `addons/dk_voxel/src/voxel_world.h/.cpp`
- Test: `tests/run_tests.gd`

**Interfaces:**
- Produces:
  - `VoxelWorld.save_directory: String = ""`.
  - `VoxelWorld.save() -> Error`: writes every modified loaded chunk and every `store` entry, then clears their `modified` flags.
- File per chunk: `<save_directory>/<x>_<y>_<z>.dkvc`, written with `FileAccess::open_compressed(path, WRITE, COMPRESSION_ZSTD)`. Contents:
  - `u32 magic 'DKVC'`
  - `u16 version = 1`
  - `f32 cell_size`
  - `String generator class name` (`store_pascal_string`; `""` if none)
  - `u32 n`, then `n` bytes from `serialize_chunk`

  `DirAccess::make_dir_recursive_absolute` creates the directory on the first write.
- With `save_directory` set:
  - unloading a modified chunk writes its file instead of the memory store `// ponytail: unload writes on the main thread; move to a writer thread if hitches show`;
  - generate jobs (and bounded `fill_all`) read a chunk's file when it exists;
  - a header mismatch (magic, version, cell size, generator class) calls `push_error` with the path and falls back to the generator. The bad file is not overwritten unless that chunk is edited again.
- Freeing or `reset()`ing a world with `save_directory` set flushes modified chunks first (the same path as unload). `reset()` never deletes files.

- [ ] **Step 1: Add the failing Godot tests**

- **`save round trip`:**
  - endless world, `save_directory = "user://dkv_test_save"` (cleared at test start);
  - dig as in Task 5, call `save()`, free the world;
  - a new world with the same settings and directory, once ready, shows the crater (`y < before - 2.5`).
- **`free without save keeps edits`:** the same, but `queue_free()` without `save()`. The crater is still there in the next world.
- **`cell size mismatch rejected`:** a new world with `cell_size = 1.0` and the same directory loads without the crater (ground within 1 m of the original height). The error is expected; print it as `ok` text.
- **`bounded world loads its save`:** bounded `Vector3i(6, 4, 6)` world with slab and save directory; dig, `save()`, then rebuild (new world, same slab, same dir). `dig.volume` replayed against it removes about 0 (`< 1.0`), because the crater is already there.

- [ ] **Step 2: Run the Godot tests. Expected: FAIL.**
- [ ] **Step 3: Implement saving as above.**
- [ ] **Step 4: Run the core and Godot tests. Expected: both pass.**
- [ ] **Step 5: Commit** — `git commit -m "feat: save edited chunks to disk"`

---

### Task 7: Build integration for games, and the README

**Files:**
- Create: `addons/dk_voxel/SConscript`
- Modify: `addons/dk_voxel/SConstruct`, `addons/dk_voxel/README.md`

**Interfaces:**
- `SConscript` returns `(sources, include_dir)`. `sources` is every addon `.cpp` except `register_types.cpp`'s entry function, which moves to `entry.cpp` (`dk_voxel_library_init`) so games can leave it out. `include_dir` is `src/`. The addon's own SConstruct builds `sources + ["src/entry.cpp"]` through it.
- A game's extension init calls `dk_voxel_register_types()` and then `GDREGISTER_CLASS(MyGenerator)`. `MyGenerator : VoxelGenerator` returns its `dkv::Generator` from `create_generator()`.

- [ ] **Step 1: Write `SConscript` and switch `SConstruct` to it. Run `./build.sh --test` and the Godot tests. Expected: both pass, which shows the shared source list builds a working extension.**
- [ ] **Step 2: Rewrite the README's "Using it" section**

Cover:
- properties (`cell_size`, `bounds_size`, `generator`, `material_set`, `save_directory`);
- `VoxelViewer`;
- `terrain_edited` and `is_area_ready`;
- "Writing a generator for your game": the submodule, the SConstruct lines that call the SConscript, a 15-line `FlatGenerator` example, and registration.

Keep the Bananza findings section. Add the 8 km limit and the double-precision upgrade path under "Where this engine differs".

- [ ] **Step 3: Commit** — `git commit -m "build: shared SConscript for games; README for the new API"`
