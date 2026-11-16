# Textures and Modularity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a voxel world's materials carry real image files through Godot resources with no game code, make world-gen rules read as named rules instead of one 48-line function, and give `addons/dk_voxel/src/` a seam between a Godot-free core and a per-concern node side.

**Architecture:** Three independent changes, in this order and for this reason: prove the tree is byte-identical first, move code while it is provably identical, then change behaviour. Textures become two `Texture2DArray`s (albedo, normal) packed by material id and triplanar-sampled, deleting the procedural noise path. World gen keeps its exact arithmetic but is rebuilt from named rules, a shared noise header and a correctness-first column cache.

**Tech Stack:** C++17, Godot 4.7 GDExtension (godot-cpp pinned to `507ed9d840c01a3c5b2a39af8bb4000bfac30bf5`), SCons, GDScript for the Godot-side tests, GLSL for the terrain shader.

**Spec:** `docs/superpowers/specs/2026-11-16-textures-and-modularity-design.md`

## Global Constraints

- Godot 4.7, godot-cpp pinned in `build.sh`, C++17, Jolt physics, SCons; Linux x86_64 and macOS arm64.
- `src/core/` stays free of Godot so host-side `selftest` keeps building it.
- Voxel format unchanged: 2 bytes per voxel, optional Hermite per 8³ block, `CHUNK` 16³ cells, `BLOCK` 8³.
- Save file header unchanged: magic `DKVC`, version 1, cell size, generator class name.
- Generated demo terrain must stay byte-identical until Task 8, and Task 8 must not change it either.
- Mesher and volume behaviour are not targets: seams stay bit-exact for any LOD mix, ms/chunk must not get worse.
- `MATERIAL_SLOTS` is 32; `pattern_scale` is tiles per metre.
- `src/core/` is the Godot-free boundary; the `dkv` namespace is used on both sides of it (`dkv::Volume`
  and `dkv::WorldState` are both legitimate), so namespace membership is not what keeps the core portable.
  `selftest` compiling `src/core/*.cpp` with no Godot headers is what keeps it portable.

**Baseline, captured on the pre-refactor tree — every task must keep these passing:**

- `./build/selftest` → `ALL CORE TESTS PASSED`, and the generator block prints
  `generate 14 ms; 256 chunks: lod0 147492 tris 0.42 ms/chunk, mixed 101506, lod2 69734 tris 0.27 ms/chunk`
- `godot --headless --path . -s tests/run_tests.gd` → `ALL GODOT TESTS PASSED`

**Environment set-up, once** (godot-cpp is not checked in):

```sh
# godot-cpp is already cloned at .worktrees/engine-core-work/addons/dk_voxel/thirdparty/godot-cpp
mkdir -p addons/dk_voxel/thirdparty && : > addons/dk_voxel/thirdparty/.gdignore
rsync -a --exclude='.git/' .worktrees/engine-core-work/addons/dk_voxel/thirdparty/godot-cpp addons/dk_voxel/thirdparty/
godot --headless --import --path .   # writes .godot/extension_list.cfg; the trailing backtrace is harmless noise
```

Three commands are used by every task. Run them from the repo root:

```sh
# 1. the Godot-free core tests
cd addons/dk_voxel && scons -j8 selftest && ./build/selftest; cd ../..

# 2. rebuild the extension the Godot tests load
cd addons/dk_voxel && scons -j8 platform=macos arch=arm64 target=template_debug; cd ../..

# 3. the Godot suite (grep the marker: godot may exit non-zero on shutdown noise)
godot --headless --path . -s tests/run_tests.gd 2>&1 | grep -E "ALL GODOT TESTS PASSED|FAILED"
```

## Review Focus

Inputs a user will actually hit that no existing test covers, and where the test for each lives:

1. **A material def with no textures at all.** A game that only sets colours must render as ground, not black or undefined. → Task 6, packing a set of bare defs, and Task 7's fallback layer.
2. **A texture that is not the array's size** (tiny 1×1, or larger than the array). The resize must not crash or divide by zero, and the array must still build. → Task 6.
3. **A voxel carrying a material id at or above the cap**, e.g. 40. It must not wrap onto material 1 and must not crash. → Task 6.
4. **Editing a material after `material_set` is assigned** (the inspector case): swapping a def's texture, or replacing the set's `materials` array, must reach the shader. → Task 6.
5. **No material set and no generator at all.** The arrays must still bind and a world must still mesh.
   → Task 6's no-set assertion and the existing `w2` world, which has no `material_set` when Task 6 first
   builds it.

---

### Task 1: Voxel-content hash in `selftest` (the safety net)

This lands first, on the untouched tree. Its value is recorded before any code moves, which is what makes every later move provably neutral.

**Files:**
- Modify: `addons/dk_voxel/src/selftest.cpp`

**Interfaces:**
- Produces: `static uint64_t hash_volume(const Volume &v)` and `static const uint64_t DEMO_WORLD_HASH`, used by Task 8's verification.

- [ ] **Step 1: Add the hash helper and print it**

Above `int main()`, add (FNV-1a over every chunk's serialised bytes, in chunk order, so it covers both the voxel values and the uniform/dense block layout):

```cpp
// FNV-1a over every chunk's serialised bytes. Any change to generated terrain, or to the block layout the
// core picks, moves this number.
static uint64_t hash_volume(const Volume &v) {
	uint64_t h = 1469598103934665603ull;
	for (int z = v.bmin[2]; z < v.bmax[2]; z++)
		for (int y = v.bmin[1]; y < v.bmax[1]; y++)
			for (int x = v.bmin[0]; x < v.bmax[0]; x++)
				for (uint8_t c : v.serialize_chunk(x, y, z)) { h ^= c; h *= 1099511628211ull; }
	return h;
}
```

In the `printf("generator\n")` block, after `v.fill_all(&g, 8);`, add:

```cpp
printf("  world hash %016llx\n", (unsigned long long)hash_volume(v));
```

- [ ] **Step 2: Run it and record the value**

Run: `cd addons/dk_voxel && scons -j8 selftest && ./build/selftest`
Expected: `ALL CORE TESTS PASSED` plus a line `  world hash <16 hex digits>`. Write that number down; it is `DEMO_WORLD_HASH`.

- [ ] **Step 3: Pin it**

Next to `static const float ID[9]`, add the recorded value:

```cpp
static const uint64_t DEMO_WORLD_HASH = 0x0000000000000000ull; // recorded on the pre-refactor tree
```

and in the generator block, after collecting the soups:

```cpp
CHECK(hash_volume(v) == DEMO_WORLD_HASH, "world hash %016llx, want %016llx",
		(unsigned long long)hash_volume(v), (unsigned long long)DEMO_WORLD_HASH);
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd addons/dk_voxel && scons -j8 selftest && ./build/selftest`
Expected: `ALL CORE TESTS PASSED`, no `FAIL` line.

- [ ] **Step 5: Commit**

```bash
git add addons/dk_voxel/src/selftest.cpp
git commit -m "test(core): pin a hash of generated demo terrain"
```

---

### Task 2: Split `voxel_core.h` and move the mesher and volume into `src/core/`

Pure code movement. No behaviour, no edits inside the moved bodies beyond includes.

**Files:**
- Create: `addons/dk_voxel/src/core/voxel_types.h`, `addons/dk_voxel/src/core/voxel_generator.h`, `addons/dk_voxel/src/core/voxel_volume.h`
- Move: `addons/dk_voxel/src/volume.cpp` → `addons/dk_voxel/src/core/voxel_volume.cpp`; `addons/dk_voxel/src/mesher.cpp` → `addons/dk_voxel/src/core/voxel_mesher.cpp`
- Delete: `addons/dk_voxel/src/voxel_core.h`
- Modify: `addons/dk_voxel/SConscript`, `addons/dk_voxel/SConstruct`, and the includes of `demo_generator.h`, `demo_generator.cpp`, `voxel_resources.h`, `voxel_world.h`, `voxel_world.cpp`, `selftest.cpp`

**Interfaces:**
- Produces: `core/voxel_types.h` (Vec3, BLOCK, CHUNK, UNBREAKABLE, to_u8, sdf_to_density, floordiv, Voxel, Block, ChunkKey, ChunkKeyHash, MeshData, Shape, Mode, EditResult, RayHit), `core/voxel_generator.h` (`dkv::Generator`), `core/voxel_volume.h` (`dkv::Volume`). Every later task includes these.

- [ ] **Step 1: Create `core/voxel_types.h`**

Take `voxel_core.h`'s contents and split at two lines: everything before `struct Generator` (lines 1–83, the technique comment, `Vec3`, the constants, the density helpers, `Voxel`, `Block`, `ChunkKey`, `ChunkKeyHash`, `MeshData`, `Shape`, `Mode`) plus `EditResult` and `RayHit` (lines 92–102). Keep the namespaces and the comments verbatim. Includes: `<algorithm>`, `<array>`, `<cmath>`, `<cstdint>`, `<memory>`, `<vector>`. Drop `<shared_mutex>` and `<unordered_map>` (they belong to `Volume` now).

- [ ] **Step 2: Create `core/voxel_generator.h`**

`struct Generator` moved verbatim, including its comment, with `#include "voxel_types.h"` above it.

- [ ] **Step 3: Create `core/voxel_volume.h`**

`class Volume` moved verbatim, including every private helper and its comments, with `#include "voxel_generator.h"`, `<memory>`, `<shared_mutex>`, `<unordered_map>`, `<vector>`.

- [ ] **Step 4: Move the two .cpp files**

`git mv addons/dk_voxel/src/volume.cpp addons/dk_voxel/src/core/voxel_volume.cpp` and `git mv addons/dk_voxel/src/mesher.cpp addons/dk_voxel/src/core/voxel_mesher.cpp`; change both `#include "voxel_core.h"` to `#include "voxel_volume.h"`. Nothing else in either file changes.

- [ ] **Step 5: Point every other include at the new headers**

`demo_generator.h/.cpp`, `voxel_world.h`, `voxel_resources.h` → `#include "core/voxel_volume.h"` (or `core/voxel_generator.h` where only `Generator` is needed). `selftest.cpp` → `#include "core/voxel_volume.h"` and `#include "core/voxel_generator.h"`. `voxel_world.cpp` and the two `.cpp` files above keep including what they use.

- [ ] **Step 6: Update the build lists**

`SConscript`'s tuple becomes `("core/voxel_mesher.cpp", "core/voxel_volume.cpp", "demo_generator.cpp", "register_types.cpp", "voxel_resources.cpp", "voxel_world.cpp")`. `SConstruct`'s selftest program sources become `["build/test/core/voxel_volume.cpp", "build/test/demo_generator.cpp", "build/test/core/voxel_mesher.cpp", "build/test/selftest.cpp"]`.

- [ ] **Step 7: Verify nothing changed**

Run all three commands from Global Constraints.
Expected: `ALL CORE TESTS PASSED` with the same `147492 tris` and the same `world hash`; `ALL GODOT TESTS PASSED`.

- [ ] **Step 8: Commit**

```bash
git add -A addons/dk_voxel
git commit -m "refactor(core): split voxel_core.h into types, generator and volume"
```

---

### Task 3: Move the node's data into `WorldState`

Mechanical again: `VoxelWorld`'s private members become `dkv::WorldState`, reached through a `unique_ptr`, so the header games include stops showing the queue and the chunk table.

**Files:**
- Create: `addons/dk_voxel/src/voxel_world_state.h`
- Modify: `addons/dk_voxel/src/voxel_world.h`, `addons/dk_voxel/src/voxel_world.cpp`, `addons/dk_voxel/SConscript` (add nothing — a header needs no entry)

**Interfaces:**
- Produces: `dkv::WorldState` with the nested `Chunk`, `Viewer`, `StoredChunk`, `Job`, `Result` types and every data member, plus `int ncx() const`, `int ncy() const`, `int ncz() const`, `int chunk_index(int x, int y, int z) const`, `Transform3D chunk_transform(const Transform3D &gx, const WorldState::Chunk &c) const`. `VoxelWorld` keeps `std::unique_ptr<dkv::WorldState> st;` and `Transform3D gx() const`.

- [ ] **Step 1: Create `voxel_world_state.h`**

Move, verbatim, `VoxelWorld`'s five nested structs and every data member (from `struct Chunk` through `double stat_ms`) into `namespace dkv { struct WorldState { ... }; }`, plus `ncx/ncy/ncz/chunk_index/chunk_transform` as member functions. Its includes are `core/voxel_volume.h` and the Godot headers the members need (`material.hpp`, `ref.hpp`, `packed_byte_array.hpp`, `transform3d.hpp`, `vector3.hpp`, `vector3i.hpp`, `string.hpp`), plus `<condition_variable>`, `<deque>`, `<memory>`, `<mutex>`, `<queue>`, `<thread>`, `<unordered_map>`, `<vector>`. Forward-declare the Godot-side resource types (`VoxelGenerator`, `VoxelMaterialSet`) above it rather than including `voxel_resources.h`, so the header stays a leaf.

- [ ] **Step 2: Shrink `voxel_world.h`**

`VoxelWorld`'s private section keeps only `std::unique_ptr<dkv::WorldState> st;`, `gx()`, the method declarations that stay node methods, and `_notification`/`_bind_methods`. Every reference in the remaining declarations changes from `Chunk`/`Job`/`Result` to `dkv::WorldState::Chunk` etc., or the declaration moves into the .cpp as a free function in `namespace dkv`.

- [ ] **Step 3: Mechanically rewrite `voxel_world.cpp`**

Every bare member (`chunks`, `chunk_lookup`, `store`, `vol`, `jobs`, `results`, `pending`, `qm`, `qcv`, `workers`, `quit`, `generation`, `need_scan`, `stat_*`, `active_viewers`, `active_generator`, `material`, `generator`, `material_set`, `cell_size`, `bounds_size`, `threads`, `lod1_distance`, `lod2_distance`, `collision_distance`, `collision_layer`, `save_directory`, `generator_class`) becomes `st->member`. `ncx()` etc. become `st->ncx()`. `chunk_transform(c)` becomes `st->chunk_transform(gx(), c)`.

- [ ] **Step 4: Verify**

Run all three commands.
Expected: identical selftest numbers and hash; `ALL GODOT TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add -A addons/dk_voxel
git commit -m "refactor(node): hold VoxelWorld state behind a unique_ptr"
```

---

### Task 4: Extract `voxel_chunks.cpp` and `voxel_save.cpp`

`voxel_world.cpp` is still carrying save-file I/O and RID lifetime. Both leave as free functions over `WorldState &`. Behaviour is unchanged; the only code that moves between functions is `apply()`'s upload half.

**Files:**
- Create: `addons/dk_voxel/src/voxel_chunks.cpp`, `addons/dk_voxel/src/voxel_save.cpp`
- Modify: `addons/dk_voxel/src/voxel_world.cpp`, `addons/dk_voxel/src/voxel_world.h`, `addons/dk_voxel/SConscript`

**Interfaces:**
- Consumes: `dkv::WorldState`, `VoxelWorld::gx()`.
- Produces, all in `namespace dkv`, declared in `voxel_world_state.h` (or a small `voxel_internal.h` if that header gets crowded). `voxel_world_state.h` needs `class VoxelWorld;` forward-declared above them, since the Godot classes are in the global namespace:

```cpp
// voxel_chunks.cpp
void free_chunk(WorldState &st, WorldState::Chunk &c, bool body_only = false);
// Uploads one chunk's mesh and, when `collision`, its static body. False when the mesh had no surface.
bool upload_chunk(WorldState &st, VoxelWorld &world, WorldState::Chunk &c, const MeshData &m, const Transform3D &xf, bool collision);

// voxel_save.cpp
String chunk_file_path(const ChunkKey &key, const String &dir);
bool write_chunk_file(WorldState &st, const ChunkKey &key, const uint8_t *data, size_t size);
bool read_chunk_file(WorldState &st, const ChunkKey &key, const String &dir, const String &gen_class, std::vector<uint8_t> &out);
void store_chunk(WorldState &st, const ChunkKey &key, bool try_disk = true);
Error save_world(WorldState &st);
```

- [ ] **Step 1: Move `voxel_chunks.cpp` out**

Move `free_chunk` and the part of `apply()` that writes `RenderingServer` arrays, `mesh_surface_set_material`, `instance_create2`, and the `PhysicsServer3D` body, into `upload_chunk`. `apply()` keeps its version/queue bookkeeping and calls `upload_chunk(*st, *this, c, r.mesh, st->chunk_transform(gx(), c), r.job.collision)`, then, when that returns false, `free_chunk(*st, c)` plus the `lod`/`collision` assignment it does today. `DKVC_MAGIC`/`DKVC_VERSION` stay with the save code, not here.

- [ ] **Step 2: Move `voxel_save.cpp` out**

Move `chunk_file_path`, `write_chunk_file`, `read_chunk_file`, `store_chunk`, and `VoxelWorld::save()`'s body (as `save_world`). `VoxelWorld::save()` becomes `{ return save_world(*st); }`. Move `DKVC_MAGIC` and `DKVC_VERSION` into this file.

- [ ] **Step 3: Add both to `SConscript`'s source tuple**

- [ ] **Step 4: Verify**

Run all three commands. Pay particular attention to the Godot suite's save round-trip checks (`save round trip restores the crater`, `bounded world loads its saved slab`, `free without save keeps edits`, `short body falls back to generated terrain`, `failed reset save keeps the edit`) — they are the only thing covering this move.
Expected: identical selftest numbers and hash; `ALL GODOT TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add -A addons/dk_voxel
git commit -m "refactor(node): split chunk upload and save I/O out of voxel_world.cpp"
```

---

### Task 5: Extract `voxel_stream.cpp`

The last of the three big blocks: viewers, scan, endless streaming and the worker pool.

**Files:**
- Create: `addons/dk_voxel/src/voxel_stream.cpp`
- Modify: `addons/dk_voxel/src/voxel_world.cpp`, `addons/dk_voxel/src/voxel_world.h`, `addons/dk_voxel/SConscript`

**Interfaces:**
- Consumes: `dkv::WorldState`, `VoxelWorld::gx()`, `upload_chunk`, `free_chunk`, `store_chunk`, `read_chunk_file`.
- Produces, in `namespace dkv`:

```cpp
void update_viewers(WorldState &st, VoxelWorld &world);
void scan(WorldState &st, VoxelWorld &world);
void stream_endless(WorldState &st, VoxelWorld &world);
void worker(WorldState &st);
void start_workers(WorldState &st, VoxelWorld &world);
void stop_workers(WorldState &st);
```

- [ ] **Step 1: Move the six functions**

`update_viewers`, `scan`, `stream_endless`, `worker`, `start_workers`, `stop_workers` move verbatim with `st.` prefixes for members. `VoxelWorld::_process` calls `dkv::update_viewers(*st, *this)` and `dkv::scan(*st, *this)`; `_ready` and `reset()` call the two worker functions.

- [ ] **Step 2: Add it to `SConscript`'s source tuple**

- [ ] **Step 3: Verify**

Run all three commands.
Expected: identical selftest numbers and hash; `ALL GODOT TESTS PASSED`, including the endless-stream, teleport-unload and collision-clamp checks that live only in the Godot suite.

- [ ] **Step 4: Commit**

```bash
git add -A addons/dk_voxel
git commit -m "refactor(node): split streaming and the worker pool out of voxel_world.cpp"
```

---

### Task 6: Materials and texture packing (C++ side)

Real image files reach the shader through resources. The shader itself still uses the noise path until Task 7, so this task's tests are all headless.

**Files:**
- Create: `addons/dk_voxel/src/voxel_materials.h`, `addons/dk_voxel/src/voxel_materials.cpp`
- Modify: `addons/dk_voxel/src/voxel_resources.h`, `addons/dk_voxel/src/voxel_resources.cpp`, `addons/dk_voxel/src/voxel_world.cpp`, `addons/dk_voxel/src/voxel_world_state.h`, `addons/dk_voxel/src/register_types.cpp`, `addons/dk_voxel/SConscript`, `tests/run_tests.gd`

**Interfaces:**
- Consumes: `dkv::WorldState` (`material`, `material_set`, `vol`).
- Produces:

```cpp
// voxel_materials.h
namespace dkv { inline constexpr int MATERIAL_SLOTS = 32; }

class VoxelMaterialDef : public Resource {          // moved from voxel_resources.h
	Ref<Texture2D> albedo, normal;                  // new; null = the packed fallback layer
	// every existing setter now ends in emit_changed();
};

class VoxelMaterialSet : public Resource {          // moved from voxel_resources.h
	// set_materials() now ends in emit_changed(); the set re-emits each def's changed signal
};

namespace dkv {
Ref<VoxelMaterialDef> material_def(const Ref<VoxelMaterialSet> &set, int id);
void apply_hardness(WorldState &st);   // the old VoxelWorld::apply_hardness, same rules
void apply_material_set(WorldState &st); // packs albedo_tex + normal_tex, pushes color_a/color_b/params
}
```

Shader parameters written: `albedo_tex` and `normal_tex` as `Ref<Texture2DArray>`, plus today's `color_a`, `color_b`, `params`.

- [ ] **Step 1: Write the failing tests in `tests/run_tests.gd`**

Insert after the existing "material uniforms fill 32 slots" block. `check()` prints and counts failures, so no framework is needed:

```gdscript
	# Textures reach the shader as one array layer per material id, from real Texture2D resources.
	var tex_world := VoxelWorld.new()
	var shader_mat := ShaderMaterial.new()
	shader_mat.shader = shader
	var albedo_img := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	albedo_img.fill(Color(1.0, 0.0, 0.0))
	var albedo_tex := ImageTexture.create_from_image(albedo_img)
	var def0 := VoxelMaterialDef.new()
	def0.albedo = albedo_tex
	var tex_defs: Array[VoxelMaterialDef] = [def0]
	var tex_set := VoxelMaterialSet.new()
	tex_set.materials = tex_defs
	tex_world.material = shader_mat
	tex_world.material_set = tex_set
	var packed: Texture2DArray = shader_mat.get_shader_parameter("albedo_tex")
	check(packed != null and packed.get_layers() == 32, "albedo packs 32 layers (%s)" % [packed])
	if packed != null:
		var layer0 := packed.get_layer_data(0)
		check(layer0 != null and layer0.get_pixel(0, 0).r > 0.9 and layer0.get_pixel(0, 0).g < 0.1,
				"layer 0 is the def's own texture")

	# Swapping the texture in place repacks, and so does replacing the set's materials array.
	var green_img := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	green_img.fill(Color(0.0, 1.0, 0.0))
	def0.albedo = ImageTexture.create_from_image(green_img)
	packed = shader_mat.get_shader_parameter("albedo_tex")
	if packed != null:
		check(packed.get_layer_data(0).get_pixel(0, 0).g > 0.9, "editing a def's texture reaches the shader")
	var def0b := VoxelMaterialDef.new()
	def0b.albedo = albedo_tex
	var swapped: Array[VoxelMaterialDef] = [def0b]
	tex_set.materials = swapped
	packed = shader_mat.get_shader_parameter("albedo_tex")
	if packed != null:
		check(packed.get_layer_data(0).get_pixel(0, 0).r > 0.9, "replacing materials repacks")

	# Odd sizes must not break packing, and a def with no textures must still produce a layer.
	var tiny_img := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	tiny_img.fill(Color(0.2, 0.4, 0.6))
	var big_img := Image.create(512, 512, false, Image.FORMAT_RGBA8)
	big_img.fill(Color(0.6, 0.2, 0.2))
	var bare := VoxelMaterialDef.new()
	var odd: Array[VoxelMaterialDef] = []
	for i in 33: # one past the cap: packed to 32, no error, no crash
		var d := VoxelMaterialDef.new()
		d.albedo = ImageTexture.create_from_image(tiny_img if i % 2 == 0 else big_img)
		odd.append(d)
	odd[0] = bare
	var odd_set := VoxelMaterialSet.new()
	odd_set.materials = odd
	var odd_world := VoxelWorld.new()
	odd_world.material = shader_mat
	odd_world.material_set = odd_set
	packed = shader_mat.get_shader_parameter("albedo_tex")
	check(packed != null and packed.get_layers() == 32, "mixed sizes and an over-long set still pack 32 layers")

	# An id past the cap is legal voxel data: it must mesh and render without error and must not wrap.
	var over := VoxelWorld.new()
	over.bounds_size = Vector3i(2, 2, 2)
	over.cell_size = 0.5
	root.add_child(over)
	over.build_box(Transform3D(Basis(), Vector3(2, 2, 2)), Vector3(1.5, 1.5, 1.5), 40)
	var far_hit := over.raycast(Vector3(2, 10, 2), Vector3.DOWN, 30.0)
	check(not far_hit.is_empty() and far_hit.material == 40, "an id past the cap survives the edit and the mesh (material %d)" % far_hit.get("material", -1))
	over.free()

	# The shader's own MATERIAL_SLOTS literal must agree with what the C++ packs.
	var shader_text := FileAccess.get_file_as_string("res://addons/dk_voxel/voxel_terrain.gdshader")
	var slots_re := RegEx.create_from_string("MATERIAL_SLOTS\\s*=\\s*(\\d+)")
	var found := slots_re.search(shader_text)
	check(found != null and packed != null and found.get_string(1).to_int() == packed.get_layers(),
			"the shader's MATERIAL_SLOTS matches the packed array layer count (%s vs %s)" % [found, packed])

	# A world with no material set at all still gets bindable arrays.
	var bare_world := VoxelWorld.new()
	var bare_mat := ShaderMaterial.new()
	bare_mat.shader = shader
	bare_world.material = bare_mat
	var bare_packed: Texture2DArray = bare_mat.get_shader_parameter("albedo_tex")
	check(bare_packed != null and bare_packed.get_layers() == 32, "no material set still packs 32 layers")
	bare_world.free()
	tex_world.free()
	odd_world.free()
```

The `VoxelMaterialDef` setters and `VoxelMaterialSet::set_materials` do not emit anything yet, and `albedo`/`normal` do not exist, so this block must fail.

- [ ] **Step 2: Run the tests to verify they fail**

Run the rebuild and test commands.
Expected: parse errors (`Invalid set index 'albedo'`) — the block cannot run yet.

- [ ] **Step 3: Move and extend the resources**

Move `VoxelMaterialDef` and `VoxelMaterialSet` into `voxel_materials.h`. `VoxelMaterialDef` gains `Ref<Texture2D> albedo, normal;` with `set_albedo`/`get_albedo`, `set_normal`/`get_normal`, and a `_bind_methods` entry for each (`PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"`). Every setter in the class ends with `emit_changed();`. `VoxelMaterialSet::set_materials` ends with `emit_changed();`, and to keep def signals alive the set connects each def's `changed` to its own `emit_changed` in `set_materials`, disconnecting the previous array first.

- [ ] **Step 4: Implement packing in `voxel_materials.cpp`**

`apply_material_set(WorldState &st)`:

- Resolve `ShaderMaterial *sm = Object::cast_to<ShaderMaterial>(st.material.ptr())`; return when null.
- Always push `color_a`, `color_b` and `params` (they are cheap and are what the existing test checks).
- Build both arrays: for `id` in `0 .. MATERIAL_SLOTS-1`, take `material_def(st.material_set, id)`'s `albedo`/`normal` image. Missing texture → the fallback layer. Resize every layer to the same square size (the constant `TEXTURE_ARRAY_SIZE`, 256) with `Image::resize(..., Image::INTERPOLATE_LANCZOS)`, convert to `FORMAT_RGBA8`, and `generate_mipmaps()` on a copy so the source resource is not mutated. `Texture2DArray::create_from_images()`.
- Fallbacks: `albedo` = a 64×64 near-white value-noise grain built once in a `static` (so colour-only materials keep the demo's speckle); `normal` = flat `(128, 128, 255)`.
- Skip the arrays when the source image RIDs and count are unchanged since the last push (store that fingerprint in `WorldState`); push otherwise. Push with `sm->set_shader_parameter("albedo_tex", albedo_array)` and `"normal_tex"`.
- Report, once per push, `UtilityFunctions::push_error` when `set->get_materials().size() > MATERIAL_SLOTS`.

`apply_hardness(WorldState &st)` is the existing `VoxelWorld::apply_hardness` body over `st`.

- [ ] **Step 5: Wire it up**

`VoxelWorld::set_material_set` stores the ref, calls `apply_hardness(*st)` and `apply_material_set(*st)`, and connects/disconnects the set's `changed` to a slot that runs both. `VoxelWorld::set_material` stores the ref and calls `apply_material_set(*st)`. `ensure_volume` and `reset()` keep calling `apply_hardness(*st)` where they called the old method. `register_types.cpp` includes `voxel_materials.h` alongside `voxel_resources.h`.

- [ ] **Step 6: Run the tests to verify they pass**

Run the rebuild and test commands.
Expected: the new `check` lines all say `ok`, and `ALL GODOT TESTS PASSED`. If `Texture2DArray` cannot be created under the headless dummy renderer, the fallback is to assert only that the parameter is a `Texture2DArray` with `get_layers() == 32`; say so in the commit message rather than weakening the packing test.

- [ ] **Step 7: Commit**

```bash
git add -A addons/dk_voxel tests/run_tests.gd
git commit -m "feat(materials): pack real textures into shader arrays from resources"
```

---

### Task 7: Rewrite the terrain shader and give the demo two real textures

**Files:**
- Modify: `addons/dk_voxel/voxel_terrain.gdshader`
- Create: `demo/textures/rock_albedo.png`, `demo/textures/rock_normal.png` (+ generated `.import` files)
- Modify: `demo/main.gd`, `addons/dk_voxel/README.md`

**Interfaces:**
- Consumes: `albedo_tex`, `normal_tex` from Task 6; `color_a`, `color_b`, `params`; the vertex `COLOR` encoding (`.r`/`.g` ids, `.b` blend, `.a` AO).
- Produces: a shader declaring `const int MATERIAL_SLOTS = 32;` and using it as the clamp bound.

- [ ] **Step 1: Generate the two textures**

Write a throwaway script `demo/make_textures.gd` (delete it after this step) that builds a 128×128 `Image` with `FastNoiseLite`-driven stone mottling and a second one with a flat-plus-bumps normal encoded as `(0.5 + dx, 0.5 + dy, 1.0)`, then `save_png("res://demo/textures/rock_albedo.png")` and `rock_normal.png`. Run it with `godot --headless --path . -s demo/make_textures.gd`, then `godot --headless --import --path .` and commit the PNGs with their `.import` files.

- [ ] **Step 2: Rewrite the shader**

Delete `noise_tex`, `bump`, the three-sample `surface()` and the screen-space derivative bump. Add:

```glsl
const int MATERIAL_SLOTS = 32;   // must match dk_voxel::MATERIAL_SLOTS; tests/run_tests.gd checks it
uniform sampler2DArray albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2DArray normal_tex : hint_normal, filter_linear_mipmap, repeat_enable;
uniform float normal_strength = 1.0;
```

Triplanar helper returning the albedo and the tangent-space normal for one id at one position, using `params[id].x` as tiles per metre and `pow(abs(NORMAL), 4.0)`-style weights normalised. Fragment: clamp both ids to `MATERIAL_SLOTS - 1` instead of `& 31`; sample both materials; `ALBEDO = tex.rgb * mix(color_a[id], color_b[id], dot(tex.rgb, vec3(0.333)))`; blend the two by the existing `COLOR.b` smoothstep; blend the decoded normals and apply `normal_strength`; keep `AO = COLOR.a`, `AO_LIGHT_AFFECT`, `ROUGHNESS`/`METALLIC`/`EMISSION` from `params`, and `RIM`.

- [ ] **Step 3: Point the demo at the textures**

`demo/main.gd`: delete the `NoiseTexture3D`/`FastNoiseLite` block in `_make_terrain_material()` (the `ShaderMaterial` still loads the shader), and in `_make_material_set()` assign `preload("res://demo/textures/rock_albedo.png")` and the normal to the `Rock` def, so the demo exercises the real file path rather than only the fallback.

- [ ] **Step 4: Verify headless**

Run the rebuild and test commands.
Expected: `ALL GODOT TESTS PASSED`, including the `MATERIAL_SLOTS` literal check from Task 6.

- [ ] **Step 5: Verify on both renderers**

```sh
godot --path . -- --shot=/tmp/shot_fwd.png
godot --path . --rendering-driver opengl3 -- --shot=/tmp/shot_gl.png
```

Expected: both runs print three `view N: … fps` lines and `shots saved`, and the images show terrain with a visibly textured rock material. This is the only check that compiles the shader at all; a headless run does not.

- [ ] **Step 6: Update the README**

The `material_set` bullet gains the two texture fields, says they take image files, and says to set `color_a`/`color_b` to white for an unmodified texture. Note the demo's `demo/textures/` as the example.

- [ ] **Step 7: Commit**

```bash
git add -A addons/dk_voxel demo docs
git commit -m "feat(materials): sample real albedo and normal textures, drop the noise path"
```

---

### Task 8: World gen as named rules

The last task, and the only one allowed to touch generation. Its proof is Task 1's hash, which must not move.

**Files:**
- Create: `addons/dk_voxel/src/core/noise.h`
- Move: `addons/dk_voxel/src/demo_generator.h` → `addons/dk_voxel/src/demo_terrain.h`; `addons/dk_voxel/src/demo_generator.cpp` → `addons/dk_voxel/src/demo_terrain.cpp`
- Modify: `addons/dk_voxel/src/core/voxel_volume.cpp` (only if the fill order needs a comment), `addons/dk_voxel/src/voxel_resources.cpp`, `addons/dk_voxel/src/selftest.cpp`, `addons/dk_voxel/SConscript`, `addons/dk_voxel/SConstruct`, `addons/dk_voxel/README.md`

**Interfaces:**
- Produces:

```cpp
// core/noise.h, header-only
namespace dkv {
uint32_t hash3(int x, int y, int z, uint32_t seed);
float noise3(float x, float y, float z, uint32_t seed);
float fbm(float x, float y, float z, uint32_t seed, int octaves);

// Caches the last NX x NZ columns for one (tag, x0, z0). `fresh` is true when the caller must fill it.
template <typename T, int NX, int NZ>
struct ColumnCache {
	T data[NX * NZ]{};
	uint64_t tag = ~0ull;
	int x0 = 0, z0 = 0;
	T *acquire(uint64_t key_tag, int key_x0, int key_z0, bool &fresh);
};

// demo_terrain.h
enum DemoMaterial : uint8_t { GRASS = 1, SOIL = 2, ROCK = 3, SAND = 4, GOLD = 5, BEDROCK = 6, BRICK = 7, CRYSTAL = 8 };
struct DemoTerrain : Generator { uint32_t seed = 7; float height = 64.0f; /* ... */ };
}
```

- [ ] **Step 1: Write the failing tests in `selftest.cpp`**

Append two blocks before the final `printf`:

```cpp
	printf("column cache\n");
	{
		ColumnCache<float, 8, 8> c;
		bool fresh = false;
		float *a = c.acquire(1, 0, 0, fresh);
		CHECK(fresh, "an empty cache reports fresh");
		for (int i = 0; i < 64; i++) a[i] = (float)i;
		float *b = c.acquire(1, 0, 0, fresh);
		CHECK(!fresh && b[63] == 63.0f, "the same key hits");
		c.acquire(2, 0, 0, fresh); // another generator's settings
		CHECK(fresh, "a different settings tag misses");
		c.acquire(1, 8, 0, fresh); // another column, same settings
		CHECK(fresh, "a different origin misses");
		float *d = c.acquire(1, 0, 0, fresh);
		CHECK(fresh, "a key that was overwritten is not served stale");
		for (int i = 0; i < 64; i++) d[i] = 1.0f;
		ColumnCache<float, 8, 8> other;
		CHECK(other.acquire(1, 0, 0, fresh) != nullptr && fresh, "a second cache on the same thread starts cold");
	}

	printf("demo material ids\n");
	{
		DemoTerrain g; g.seed = 1234; g.height = 32.0f;
		Volume v(4, 4, 4, 0.5f);
		v.fill_all(&g, 4);
		bool known = true;
		for (int i = 0; i < 3; i++)
			for (int y = 0; y < 64; y++) {
				uint8_t m = v.get(i * 16 + 8, y, 8).m;
				known &= m == 0 || (m >= GRASS && m <= CRYSTAL);
			}
		CHECK(known, "generated terrain uses only named materials");
	}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd addons/dk_voxel && scons -j8 selftest && ./build/selftest`
Expected: build error — `ColumnCache` and `GRASS` do not exist.

- [ ] **Step 3: Create `core/noise.h`**

Move `hash3`, `noise3` and `fbm` out of `demo_generator.cpp` verbatim, as `inline` functions in `namespace dkv`, keeping their comments. Add `ColumnCache` with the interface above; `acquire` compares `key_tag`/`key_x0`/`key_z0` against the stored key, sets `fresh` accordingly, and stores the new key.

- [ ] **Step 4: Rename the demo generator and name its materials**

`git mv` both files to `demo_terrain.*`; update `voxel_resources.cpp`, `selftest.cpp`, `SConscript` and `SConstruct` (the selftest source list). Add `enum DemoMaterial` to the header. Keep the class and its members exactly as they are so the resource binding and every test keep working.

- [ ] **Step 5: Decompose `generate_block` without changing a single value**

In `demo_terrain.cpp`:

- A `namespace { constexpr float ... }` block at the top holding today's literals under names: the height floor/continuous/mountain/mesa factors, the block-column cache size, the overhang strength and height ramp, the tunnel offset/threshold, the cavern threshold, the bedrock floor, the layer depths, and the two ore thresholds.
- `void column_heights(const DemoTerrain &, int x0, int z0, float *h)` — the 64 fbm/ridge/mesa columns, replacing the `Col` struct's body and using `ColumnCache<float, 8, 8>` keyed by `(tag = seed ^ cell bits ^ height bits, x0, z0)`.
- `float height_max(const float *h)`.
- `float cave_sdf(float wx, float wy, float wz, uint32_t seed, float sd)` — the two noise sheets and the blob field.
- `float overhang_offset(...)` — the cliff/overhang term.
- `uint8_t surface_material(float depth_m, float h, float height, float wobble)` — sand/rock/grass and the soil band, returning `DemoMaterial` values.
- `uint8_t ore_material(float wx, float wy, float wz, uint32_t seed, uint8_t base)` — gold and crystal.
- `generate_block` becomes: air when the block is above `height_max`; otherwise per voxel — sample `column_heights`, apply overhang, caves, the bedrock floor, then `surface_material` and `ore_material`, then write `{sdf_to_density(sd / cell), m}`.

The arithmetic, the order of the noise calls and the `Voxel` values must be identical; only the names and the call structure change.

- [ ] **Step 6: Run the tests to verify nothing moved**

Run all three commands.
Expected: `ALL CORE TESTS PASSED` with the *same* `world hash`, the *same* `147492 tris` / `0.42 ms/chunk`, and the two new blocks passing. `ALL GODOT TESTS PASSED`.

If the hash moved, a value changed: compare against the pre-refactor `demo_generator.cpp` in `git show HEAD:addons/dk_voxel/src/demo_generator.cpp` rather than adjusting the constant. **`DEMO_WORLD_HASH` is never updated in this task.**

- [ ] **Step 7: Update the README's generator section**

Point the example at `core/voxel_generator.h`, name `core/noise.h`'s helpers as reusable, and note that the demo's `MATERIALS` array order matches `dkv::DemoMaterial`.

- [ ] **Step 8: Commit**

```bash
git add -A addons/dk_voxel
git commit -m "refactor(demo): rebuild world gen from named rules and a keyed column cache"
```

---

## Self-review notes

- Task order is forced in one place: Task 1 before everything, because the hash is the only proof the moves in Tasks 2–5 and 8 were neutral.
- Every task's verification runs both suites. The Godot suite is the only coverage for streaming, saves and collision, which Tasks 3–5 move; the core suite is the only coverage for the mesher, which nothing touches.
- Task 6's tests are the Review Focus items 1, 2, 3, 4 and 5; Task 7's `--shot` run is Review Focus item 1's visual half.
