# Handoff: practically infinite view distance (far-field LOD levels)

Branch `demo-polish`, uncommitted. Status: working and visually verified; one cleanup and a few checks left.

## Goal
The endless (procedural) world only showed terrain to ~178 m (the camera-fallback radius `lod2_distance + chunk`).
Make the view distance practically infinite.

## Research (what was decided and why)
- **Bananza** (local romfs index): voxel fields are bounded; far scenery is hand-made static models
  (`*DistantView*`, `Far_*LevelModel*` bfres). Not applicable to a generated endless world.
- **BotW**: terrain is a heightmap quadtree (public TSCB format), where each level covers 2× the area at half resolution.
  BotW's game files were not reverse-engineered.
- **voxy** (github.com/MCRcortex/voxy), Distant Horizons, godot_voxel `VoxelLodTerrain`: nested LOD levels, each 2× coarser.
  voxy builds its levels by downsampling real data into a database. We instead **sample the generator directly at the coarse
  cell size**, which needs no LOD store.
- Chosen design: nested coarse copies of the world (clipmap). Each level costs about the same, so total cost grows with
  log(distance).

## What was built
New `addons/dk_voxel/src/core/voxel_far.cpp` (added to `SConscript`):
- Level 0 is the existing near field (collision, saves, raycasts, LOD 0–2), unchanged.
- Level k = 1..`far_levels`: its own endless `Volume` with cell `cell_size·2^k`, filled from the generator, meshed at
  `FAR_LOD = 1`, no collision, no saves. Reach: horizontal `view_distance·2^k`, vertical `vertical_view_distance + 2·chunk_k`.
  The vertical reach does not double per level, which keeps coarse levels thin.
- A level-k chunk covers exactly 2×2×2 level-(k−1) chunks. `update_visibility()` shows a chunk iff its parent
  "handed over" (all 8 siblings in reach and meshed) and it has not itself handed over. This means no holes during streaming:
  coarse terrain stays visible until all of its finer children are ready. It is verified while flying at 150 m/s.
- The visibility pass is dirty-tracked per level (`WorldState::need_vis` bitmask; readiness at level k dirties k and k+1;
  viewer moves dirty all).
- Meshing of far chunks is event-driven (`queue_far_mesh` on gen-complete for the 27-neighbourhood, on edits, and on level
  restream), not a full scan.
- Edits are mirrored into far levels whose cell is ≤ the edit's half-size, so craters show from afar. Smaller edits would
  only dent the coarse surface.
- Worker: `Job::level` picks the volume. Gen priority for inner coarse chunks is pushed back so near content loads first.
- New property `VoxelWorld.far_levels` (default 6 ≈ 11 km; 0 = off; applies on `reset()`). It is ignored for bounded
  worlds. `get_stats()` gains `far_chunks`.
- Demo `main.tscn`: camera `far` 600 → 20000, `fog_density` 0.0028 → 0.0004.

Files touched: `SConscript`, `voxel_far.cpp` (new), `voxel_world_state.h`, `voxel_world.cpp/.h`, `voxel_stream.cpp`,
`voxel_chunks.cpp`, `demo/main.tscn`.

## Measurements (RTX 4070 laptop, 14 worker threads)
| | before | after |
|---|---|---|
| visible distance | ~178 m | horizon (~11 km) |
| static fps (`--shot`, procedural) | 163–167 | 156–167 |
| triangles | 1.56 M | ~9 M |
| chunks loaded | 36 k | 36 k near + ~82–95 k far |
| full load to `pending == 0` | ~10 s | ~19.5 s |
| walking at 9 m/s, 240 frames | 2.7 s | 3.6 s (visibility pass ~1–2 ms/frame) |
| flying at 150 m/s, 240 frames | 5.6 s | 8.6 s (visibility pass ~8 ms/frame: every frame dirties all levels) |

Screenshots: `--shot` after_0/1, fly_mid in the session scratchpad. Mid-flight there are no holes; coarse terrain stands in
until the near field catches up.

## Done since the first handoff
- **Tests fixed and passing.** The material/texture tests read packed data through `ShaderMaterial` properties, which commit
  a174e21 stopped writing. They now use the new `VoxelWorld.get_material_array(kind)` getter (0 albedo, 1 normal,
  2 roughness, 3 AO). The uniform check is skipped headless, because the dummy renderer stores no material params.
  `./build.sh --test` and `godot --headless --path . -s tests/run_tests.gd` both pass (88 checks).
- **Level cracks fixed with skirts.** `add_skirts()` (voxel_mesher.cpp) folds every open chunk edge 2 cells into the surface
  along the vertex normal. Open edges are matched by position, because vertices are split by material and at sharp edges.
  Skirts apply to far chunks and, when far levels exist, to near chunks. `MeshData::skirt_from` keeps them out of collision.
  There is a self-test for this ("skirts").
- README documents `far_levels`.
- Cost with skirts: 11.6 M triangles (up from 9.0 M), static fps 141–168.

## Open items / next steps
1. **High altitude:** from about 400 m up, the ground below is drawn by a coarse level (cell 8 m). This is because vertical
   reach doesn't double per level. Doubling it would cost about 7× the load (deep solid chunks are expensive to generate).
   Options: per-viewer `vertical_view_distance`, a generator fast path for deep solid, or reach based on altitude.
2. Possible cost cuts:
   - Use `FAR_LOD = 2`.
   - Skirt only near chunks on the level-1 border (right now every near chunk is skirted).
   - Skip coarse chunks deep inside the finer level.
   - Dirty only the levels whose `streamed_at` changed when the viewer moves (very fast flight costs ~8 ms in the
     visibility pass).
3. Known ceilings: far copies of edits last only until that far chunk unloads; far terrain has no collision; fine generator
   features alias at distance.
4. Commit (nothing committed yet).

## Working-tree notes
- `demo/main.gd` (`profile := "terrain"`) and the `shader_parameter/*` lines in `demo/main.tscn` were **not** written by
  this work. They came from the user or the open Godot editor. Leave or revert them deliberately.
- During the session, `git stash` / `stash pop` briefly reverted the working tree. If the editor had files open, reload them.
- Fly-through test script: `/tmp/claude-1000/-home-jeremy-Development-Godot-dk-voxel-engine/a6aa89f0-e98b-44a4-916d-005ae84f844c/scratchpad/fly.gd`
  (env: `FAR`, `STEP` metres/frame, `OUT` dir). It lives in the scratchpad; copy it into `tests/` if it's worth keeping.
