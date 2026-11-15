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

```gdscript
var world := VoxelWorld.new()
world.material = my_shader_material          # see voxel_terrain.gdshader
world.material_set = my_material_set         # VoxelMaterialSet: colours and hardness per id
world.bounds_size = Vector3i(32, 8, 32)      # 16-cell chunks from the origin; zero = endless
world.cell_size = 0.5
world.generator = DemoTerrainGenerator.new() # seed and height are properties; VoxelGenerator is abstract
add_child(world)
world.reset()                                # rebuild with the current properties (also on demand)

var r := world.dig_sphere(pos, 1.6, 1)       # power 1; r.removed = {material: m³}, r.volume
world.build_sphere(pos, 1.6, 7)
world.smooth_sphere(pos, 2.4, 0.6, 1)
world.build_box(xf, half_extents, 7)         # sharp = true: exact edges and corners
world.dig_cylinder(xf, radius, half_height, 9)
var hit := world.raycast(from, dir, 14.0)    # {position, normal, material}; marches the voxels
```

Properties: `cell_size`, `bounds_size`, `generator`, `material_set`, `threads`, `lod1_distance`,
`lod2_distance`, `collision_distance`, `collision_layer`, `material`. Hardness comes from `material_set`
(255 = unbreakable), and `bounds_size`/`cell_size`/generator changes apply on the next `reset()`.
The LOD viewer is the viewport's current camera. Edits are main-thread calls; meshing runs on worker
threads and edited chunks jump the queue.

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
