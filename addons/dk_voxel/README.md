# dk_voxel

A destructible voxel terrain engine for Godot 4.7 (GDExtension, C++17). One node, `VoxelWorld`.
The technique is the one Donkey Kong Bananza ships: a sparse signed-distance grid with optional Hermite
data, meshed by dual contouring with QEF vertices and octree-collapse LOD.

```
./build.sh --test                                   # library + core self-test
godot --path ../..                                  # play the demo
godot --headless --path ../.. -s tests/run_tests.gd # engine tests (physics included)
godot --path ../.. -- --shot=/tmp/shot.png          # three staged screenshots + fps
```

## Using it

One node, `VoxelWorld`; generators are `VoxelGenerator` resources. Set the properties, `add_child`,
then `reset()`. Edits are main-thread calls; meshing runs on worker threads and edited chunks jump the
queue.

```gdscript
var world := VoxelWorld.new()
world.cell_size = 0.5                     # metres per voxel
world.bounds_size = Vector3i(32, 8, 32)   # 16³-cell chunks from the origin; zero = endless
world.generator = DemoTerrainGenerator.new()
world.material_set = my_material_set      # a VoxelMaterialDef per material id
world.material = my_shader_material       # see voxel_terrain.gdshader
world.save_directory = "user://saves/slot1/overworld"   # empty = nothing reaches disk
add_child(world)
world.reset()                             # rebuild with the current properties

var viewer := VoxelViewer.new()           # terrain streams around this node, not the camera
viewer.view_distance = 160.0              # metres
viewer.vertical_view_distance = 96.0
viewer.collision_distance = 40.0
player.add_child(viewer)
```

Properties:

- `cell_size` — metres per voxel, default 0.5.
- `bounds_size` — the world in chunks from the origin. `Vector3i.ZERO` (the default) is endless: chunks
  stream in around the viewers and unload behind them.
- `generator` — a `VoxelGenerator`; none means air. `DemoTerrainGenerator` has `seed` and `height`.
- `material_set` — a `VoxelMaterialSet`: one `VoxelMaterialDef` per material id, with `albedo` and `normal`
  textures, colour, pattern, roughness, metallic, emission and hardness. `albedo` and `normal` take ordinary
  image files: the addon packs one `Texture2DArray` layer per material id and triplanar-projects them, so a
  game only assigns resources — no C++, no shader edit, no rebuild. The two colours are a tint ramp over the
  albedo, driven by the albedo's own luminance; set both to white to show a texture unmodified, and see
  `demo/textures/` plus `demo/main.gd` for the working example. `pattern_scale` is tiles per metre. A
  material with no textures gets a built-in grain and a flat normal. The shader's `normal_strength`
  uniform scales the normal map if it reads too strong at a given tiling. Ids run to 32; a voxel carrying a
  higher id renders as the last slot, and a set with more defs than that reports an error. In a non-empty
  set an id without a def reads as entry 0; with no set, or an empty one, every id gets a zero-hardness grey
  default. Hardness 255 is unbreakable and an erase needs `power >= hardness`.
- `save_directory` — where edited chunks are written; empty keeps edits in memory only (see *Saving*).
- `material` — the terrain shader material (`voxel_terrain.gdshader`); the material set fills its uniforms.
- `threads`, `lod1_distance`, `lod2_distance`, `collision_distance`, `collision_layer` — meshing threads
  (0 = cores − 2) and LOD/collision tuning.

Changing `cell_size`, `bounds_size` or `generator` takes effect on the next `reset()`; `material_set`
applies immediately, to the hardness table and the shader uniforms.

Every `VoxelViewer` registers with the `VoxelWorld` nodes that share its `World3D`; together they decide
what is loaded, at which LOD, and with collision. With no viewer the active camera acts as one. In the
editor nothing streams and nothing is meshed.

```gdscript
var r := world.dig_sphere(pos, 1.6, 1)     # power; r.removed = {material: m³}, r.volume
world.build_sphere(pos, 1.6, 7)
world.smooth_sphere(pos, 2.4, 0.6, 1)
world.build_box(xf, half_extents, 7)       # sharp = true: exact edges and corners
world.dig_cylinder(xf, radius, half_height, 9)
var hit := world.raycast(from, dir, 14.0)  # {position, normal, material}; marches the voxels
```

### Reacting to the world

`terrain_edited(aabb, result)` fires once per edit that changed something; `aabb` is the edited shape's
box in world space and `result` is the dictionary the edit method returned (`removed` per material id in
m³, `volume`, `changed`). `is_area_ready(aabb, need_collision = true)` is false until every chunk under
`aabb` (global) is loaded and meshed at its LOD, and, with `need_collision`, has a collision body unless
it has no surface — hold a spawn, a teleport or a test on it instead of guessing frame counts. It is
also false while there is no viewer and no camera, since nothing is streaming. Bodies only exist at LOD0,
so an area reaching past the collision radius (clamped to the LOD1 band) needs `need_collision = false`.

```gdscript
world.terrain_edited.connect(_on_terrain_edited)   # audio, AI, quests, debris
while not world.is_area_ready(AABB(spawn, Vector3(32, 32, 32))):
	await get_tree().process_frame
```

### Saving

Edited chunks are written to `save_directory` when they unload and on `save()`; freeing the world flushes
them first. The file header holds the format version, `cell_size` and the generator's class name, and a
mismatch is rejected with an error (the chunk falls back to the generator). Seed and other generator
settings are not checked: give each world or seed its own directory. `reset()` never deletes files.

## Writing a generator for your game

Generators are C++: they run on worker threads and must not touch Godot. Compile the addon's sources into
your game's own GDExtension (do not enable the addon's `dk_voxel.gdextension` as well — your library
replaces it).

1. Add the addon next to your project as a submodule, so the version is pinned:

   ```sh
   git submodule add <addon-repo-url> addons/dk_voxel
   ```

2. Pull the addon's source list into your `SConstruct`, after godot-cpp has set up `env`:

   ```python
   dk_sources, dk_include = SConscript("addons/dk_voxel/SConscript", {"env": env})
   env.Append(CPPPATH=[dk_include])
   env.SharedLibrary("bin/mygame", source=dk_sources + env.Glob("src/*.cpp"))
   ```

   `dk_sources` are the addon's library `.cpp` files (`selftest.cpp` is a host-side test and is not in
   the list): they compile with your `env` and flags, and their objects land in
   `addons/dk_voxel/build/obj`. `dk_include` is the addon's `src/`, where `register_types.h`,
   `voxel_world.h` and `voxel_resources.h` live, `voxel_materials.h` holds `VoxelMaterialDef` and
   `VoxelMaterialSet`, and the Godot-free core sits under `core/` (`core/voxel_generator.h`,
   `core/voxel_volume.h`, `core/voxel_types.h`, `core/noise.h`). The addon's own entry point (`src/entry.cpp`, which
   defines `dk_voxel_library_init`) is deliberately not in the list: your game's library supplies its own
   entry symbol.

3. Subclass `VoxelGenerator` (Godot-facing) and return your `dkv::Generator` from `create_generator()`.
   The core generator runs on worker threads, so it must be `const`, thread-safe and deterministic in its
   settings and block origin — an unloaded chunk is regenerated exactly. A flat world, solid below y = 0:

   ```cpp
   #include "voxel_resources.h"

   struct Flat : dkv::Generator {
   	void generate_block(const int origin[3], float, dkv::Voxel out[512]) const override {
   		for (int z = 0; z < 8; z++)
   			for (int y = 0; y < 8; y++)
   				for (int x = 0; x < 8; x++)
   					out[(z << 6) | (y << 3) | x] =
   						origin[1] + y < 0 ? dkv::Voxel{255, 3} : dkv::Voxel{0, 0};
   	}
   };

   class FlatGenerator : public VoxelGenerator {
   	GDCLASS(FlatGenerator, VoxelGenerator)
   public:
   	std::shared_ptr<const dkv::Generator> create_generator() const override { return std::make_shared<Flat>(); }
   protected:
   	static void _bind_methods() {}
   };
   ```

   `origin` is the 8³ block's origin in cell coordinates; density 0 is air, 255 is solid, and the second
   byte is the material id. Then `world.generator = FlatGenerator.new()`, as for any resource.

   Two things in the addon are there to be copied rather than inherited: `core/noise.h` gives you `hash3`,
   `noise3`, `fbm` and a `ColumnCache` keyed on the settings and the column origin, so a per-block heightmap
   cache is correct no matter what order the engine fills blocks in; and `demo_terrain.cpp` shows a full
   generator written rule by rule — height, overhangs, caves, surface layers, ore — with every constant named
   at the top of the file. Its material ids are `dkv::DemoMaterial` in `demo_terrain.h`, in the same order as
   the demo's `MATERIALS` array.

4. Register the addon's classes and yours from your module initializer:

   ```cpp
   #include "register_types.h"
   #include <godot_cpp/godot.hpp>

   using namespace godot;

   static void init(ModuleInitializationLevel level) {
   	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
   	dk_voxel_register_types();
   	GDREGISTER_CLASS(FlatGenerator);
   }
   ```

   Then `obj.register_initializer(init)` in your `*_library_init`, as in the addon's `src/entry.cpp`.

## What the game does, and how it was established

Target: the Switch executable (`exefs/main`, decompressed and decompiled function by function with
Ghidra) and the `Voxel/` asset tree. **Observed** means read directly from data or decompiled code;
**inferred** means the evidence points there but the code path was not read end to end.

| | Finding | Evidence |
|---|---|---|
| Storage | Density grid, `VolumeType: "Sdf"`, in 8³-voxel blocks behind a one-bit-per-block header; each block is its own zstd frame. 8 bytes per voxel at runtime: u8 density (surface at 127.5, full range within about ±1 cell), two material ids, a blend weight. | Observed: `vhead.bin` / `vbody.bin` parsed for two stage parts; pool `VoxelSceneRawDataUnitHeap` has 0x1000-byte units. |
| Hermite data | A parallel grid, 9 bytes per cell = 3 edges × 3 bytes: `[t (7 bits) \| sign]`, `nx`, `ny`. `t = ((b0 >> 1) - 1) / 126`, `nz = ±sqrt(1 - nx² - ny²)`. Absent entries (`b0 < 2`) fall back to linear interpolation of the density and its gradient. Optional per part (`EnableHermite`). | Observed: `ehead.bin` / `ebody.bin`, 0x1200-byte units, decoder in the leaf builder at `0x16fc0`. |
| Vertex placement | One vertex per surface cell from a quadric error function over the cell's crossing edges: 3×3 `AᵀA` diagonalised with Jacobi rotations (5 sweeps, 0.001 off-diagonal cut-off), eigenvalues under 0.1 dropped, solved about the mass point, clamped 0.05 inside the cell. | Observed: `0x16fc0`. |
| Meshing | Dual contouring over an octree (`VoxelSceneOctreeNodeUnit`, 0x70-byte nodes): recursive cell / face / edge procedures emit quads between the four cells around each sign-changing edge. No marching-cubes or Transvoxel tables exist in the binary. | Observed: `0x1590`, `0xb9c0`, `0xbf90`, `0xdd40`; table search over the whole image. |
| LOD | Three levels per 16³-cell chunk, picked by camera distance with ±8-cell hysteresis. A level is an error threshold: an octree node whose stored QEF error is under it is treated as a leaf. `Lod1Dist/Lod2Dist` and `Lod1Value/Lod2Value` in `VoxelMeshLodParam` are those distances and thresholds. | Observed: worker at `0x20`, traversal at `0x1590`, the `.bgyml` params. |
| Materials | Per quad, the two heaviest materials among its vertices are kept and each vertex gets a blend byte. Vertex weights are trilinear corner weights × corner density. | Observed: `0x4180`, tail of `0x16fc0`. |
| Threading | `VoxelDisplayMeshUpdater%d` workers pull (chunk, priority) jobs from two queues; `VoxelGridWriterThread%d` ×2 apply edits. A chunk carries separate "geometry" and "display" dirty flags and is meshed at full detail first. | Observed: `0x20`, `0xe78`, `0x743ae8`. |
| Shading | Triplanar albedo + packed normal textures per material, texture bombing, rim light, depth-based wetness, baked field AO. | Observed in material and parameter files only; shaders not decompiled. |
| Collision | A custom voxel-mesh shape inside the physics engine with its own shape-cast and closest-point routines (`convexVoxelMeshShapeCast`, …). | Inferred from registration strings at `0x3f6420`. |
| GPU step | Compute shaders `alVoxelCreateVertex` (option `ENABLE_SKINNING`) and `alVoxelCreateIndex` (`INDEX_TYPE`) build the final buffers. | Inferred from the lookup at `0x743a00`; shader bodies unknown. |

Still unknown: voxel bytes 4–7, the exact collapse penalties (constants 100 / 5000 / 10000 in the
octree builder at `0x130a0`), and how seams between chunks of different LOD are closed.

## Where this engine differs

- Voxels are 2 bytes (density, material); blends are derived per vertex rather than authored.
- LOD collapse runs per chunk. Cells on a chunk's +faces never collapse, which is what keeps seams
  watertight for any mix of LODs but caps the saving at roughly half the triangles.
- A normal-cone test stands in for the game's topology penalties when collapsing.
- Collision is the LOD0 mesh as a Jolt trimesh per chunk, near the camera only.
- Surface patterns come from one seamless 3D noise texture instead of authored triplanar sets.
- The world is good to about 8 km from the origin. Stock single-precision Godot jitters the camera, the
  player and physics further out whatever the voxel code does. Mesh and collision vertices are relative to
  the chunk origin and core voxel coordinates are `int32`, so the upgrade path is a `precision=double`
  build of Godot and godot-cpp and nothing else. Origin shifting was considered and rejected: every game
  would have to cooperate with it.
