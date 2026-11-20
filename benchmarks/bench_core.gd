extends SceneTree
## Engine benchmark (CPU side, no rendering needed):
##   godot --headless --path . -s benchmarks/bench_core.gd
## Measures generation + meshing throughput (and how it scales with worker threads), endless-world
## streaming under a moving viewer, level loading, edit cost and re-mesh latency, raycasts, density
## queries and glb voxelizing. Prints a table and writes benchmarks/results/core_<time>.json.
## User args (after --):  --quick  fewer repetitions

const Lib := preload("res://benchmarks/bench_lib.gd")
const Util := preload("res://demo/voxel_util.gd")
const LEVEL_PATH := "res://demo/levels/mini_level.dkvl"
const TREES := ["res://demo/trees/tree_a.glb", "res://demo/trees/tree_b.glb", "res://demo/trees/tree_c.glb",
		"res://demo/trees/pine_a.glb", "res://demo/trees/pine_b.glb"]
const BOUNDS := Vector3i(16, 8, 16) # chunks: 128 m x 64 m x 128 m at 0.5 m cells

var lib: Lib
var rng := RandomNumberGenerator.new()
var quick := false


func _initialize() -> void:
	quick = "--quick" in OS.get_cmdline_user_args()
	OS.low_processor_usage_mode_sleep_usec = 0 # headless sleeps 6.9 ms per frame by default, which would floor every frame time
	lib = Lib.new(self, "core")
	lib.print_header()
	rng.seed = 1

	print("-- generation + meshing, bounded %s chunks, procedural --" % BOUNDS)
	var counts := [1, 2, 4, 8, 0] if not quick else [1, 0]
	for n in counts:
		free_world(await bounded_world(n))

	print("\n-- hand-made level (mini_level.dkvl) --")
	await level_load()

	print("\n-- endless world: first load around one viewer --")
	for far in [0, 6]:
		await endless_load(far)

	print("\n-- endless world: moving viewer --")
	for speed in [10.0, 30.0, 100.0]:
		await endless_move(speed, 6.0 if quick else 12.0)

	print("\n-- edits on a settled bounded world --")
	var ew := await bounded_world(0, false)
	await edits(ew)
	await queries(ew)
	await voxelize(ew)
	free_world(ew)

	lib.save()
	quit(0)


## Average main-thread frame time with nothing queued: the engine's per-frame bookkeeping cost.
func idle_frame_ms(frames := 120) -> float:
	await process_frame
	var t0 := Lib.now_ms()
	for i in frames:
		await process_frame
	return (Lib.now_ms() - t0) / frames


func free_world(w: VoxelWorld) -> void:
	if w.has_meta("viewer"):
		(w.get_meta("viewer") as Node).free()
	w.free()


func add_viewer(at: Vector3, dist := 160.0) -> VoxelViewer:
	var v := VoxelViewer.new()
	v.view_distance = dist
	v.position = at
	root.add_child(v)
	return v


## A bounded procedural world, timed from reset() until every chunk is meshed. threads 0 = engine default.
func bounded_world(threads: int, report := true) -> VoxelWorld:
	var w := VoxelWorld.new()
	w.cell_size = 0.5
	w.bounds_size = BOUNDS
	w.threads = threads
	var gen := DemoTerrainGenerator.new()
	gen.seed = 7
	w.generator = gen
	root.add_child(w)
	var size := Vector3(BOUNDS) * 8.0
	var v := add_viewer(size * Vector3(0.5, 1.0, 0.5), size.length())
	var t0 := Lib.now_ms()
	w.reset()
	var idle := await lib.await_idle(w)
	var ms := Lib.now_ms() - t0 if idle >= 0 else -1.0
	var s := w.get_stats()
	if report:
		lib.record("generate threads=%s" % (str(threads) if threads else "default(%d)" % s.threads), {
			"total_ms": ms, "idle_frame_ms": await idle_frame_ms(), "chunks": s.chunks, "meshes": s.meshes, "triangles": s.triangles, "bodies": s.bodies,
			"mesh_ms_per_job": s.mesh_ms, "chunks_per_s": s.chunks / (ms / 1000.0) if ms > 0 else 0.0,
		})
	w.set_meta("viewer", v)
	return w


func level_load() -> void:
	var gen := LevelTerrainGenerator.new()
	gen.level_path = LEVEL_PATH
	var w := VoxelWorld.new()
	w.cell_size = 0.5
	w.generator = gen
	w.bounds_size = Vector3i((gen.get_level_size() / (16.0 * w.cell_size)).ceil())
	root.add_child(w)
	var v := add_viewer(gen.get_level_size() * 0.5, gen.get_level_size().length())
	var t0 := Lib.now_ms()
	w.reset()
	var idle := await lib.await_idle(w)
	var s := w.get_stats()
	lib.record("level load", {"total_ms": Lib.now_ms() - t0 if idle >= 0 else -1.0, "size_m": str(gen.get_level_size()),
			"chunks": s.chunks, "meshes": s.meshes, "triangles": s.triangles, "mesh_ms_per_job": s.mesh_ms})
	w.free()
	v.free()


func endless_world(far: int) -> VoxelWorld:
	var w := VoxelWorld.new()
	w.cell_size = 0.5
	w.far_levels = far
	var gen := DemoTerrainGenerator.new()
	gen.seed = 7
	w.generator = gen
	root.add_child(w)
	return w


func endless_load(far: int) -> void:
	var w := endless_world(far)
	var v := add_viewer(Vector3(0, 40, 0))
	var t0 := Lib.now_ms()
	w.reset()
	var near_ms := -1.0
	var near := AABB(Vector3(-32, -16, -32), Vector3(64, 80, 64))
	while Lib.now_ms() - t0 < 120000.0:
		await process_frame
		if w.is_area_ready(near, false): # meshed at its LOD; collision only exists within collision_distance
			near_ms = Lib.now_ms() - t0
			break
	var idle := await lib.await_idle(w)
	var total := Lib.now_ms() - t0 if idle >= 0 else -1.0
	var s := w.get_stats()
	lib.record("endless load far_levels=%d" % far, {"spawn_ready_ms": near_ms, "total_ms": total,
			"idle_frame_ms": await idle_frame_ms(), "chunks": s.chunks, "far_chunks": s.far_chunks, "meshes": s.meshes, "triangles": s.triangles, "bodies": s.bodies})
	w.free()
	v.free()


## Flies the viewer along +x at `speed` m/s for `secs` of game time in fixed 60 Hz steps (the viewer moves
## speed/60 per frame however long the frame took), so the frame times are the main-thread cost of one game
## frame while streaming, and max_pending shows how far meshing falls behind at that speed.
func endless_move(speed: float, secs: float) -> void:
	var w := endless_world(6)
	var v := add_viewer(Vector3(0, 40, 0))
	w.reset()
	await lib.await_idle(w)
	var frames := PackedFloat64Array()
	var max_pending := 0
	var steps := int(secs * 60.0)
	var t0 := Lib.now_ms()
	var last := t0
	for i in steps:
		v.position.x += speed / 60.0
		await process_frame
		var t := Lib.now_ms()
		frames.append(t - last)
		last = t
		max_pending = maxi(max_pending, w.get_pending_count())
	var behind := AABB(v.position + Vector3(-16, -24, -16), Vector3(32, 64, 32))
	var drain := await lib.await_idle(w)
	var over := 0
	for f in frames:
		if f > 1000.0 / 60.0:
			over += 1
	lib.record("endless fly %d m/s" % int(speed), {"frames": steps, "frame_avg_ms": Lib.mean(frames),
			"frame_p50_ms": Lib.percentile(frames, 50), "frame_p99_ms": Lib.percentile(frames, 99),
			"frame_max_ms": Lib.percentile(frames, 100), "frames_over_16.7ms": over, "max_pending": max_pending,
			"drain_after_ms": drain, "area_ready_at_end": w.is_area_ready(behind, false)})
	w.free()
	v.free()


## A point on the surface, from a downward raycast at a random x/z inside the world (away from the edges).
func surface_point(w: VoxelWorld) -> Vector3:
	var size := Vector3(BOUNDS) * 8.0
	for i in 32:
		var p := Vector3(rng.randf_range(16, size.x - 16), size.y - 1, rng.randf_range(16, size.z - 16))
		var hit := w.raycast(p, Vector3.DOWN, size.y)
		if not hit.is_empty():
			return hit.position
	return size * 0.5


func edits(w: VoxelWorld) -> void:
	var n := 20 if quick else 60
	var ops := {
		"dig_sphere r=1.6": func(p: Vector3) -> void: w.dig_sphere(p, 1.6, 255),
		"dig_sphere r=4": func(p: Vector3) -> void: w.dig_sphere(p, 4.0, 255),
		"dig_sphere r=8": func(p: Vector3) -> void: w.dig_sphere(p, 8.0, 255),
		"build_sphere r=1.6": func(p: Vector3) -> void: w.build_sphere(p, 1.6, 7),
		"smooth_sphere r=2.4": func(p: Vector3) -> void: w.smooth_sphere(p, 2.4, 0.6, 255),
		"build_box 2x2x2 sharp": func(p: Vector3) -> void: w.build_box(Transform3D(Basis(Vector3.UP, 0.4), p), Vector3(1, 1, 1), 3, true),
		"dig_cylinder r=1.4 sharp": func(p: Vector3) -> void: w.dig_cylinder(Transform3D(Basis(), p), 1.4, 1.5, 255, true),
	}
	for op_name in ops:
		var op: Callable = ops[op_name]
		var call_ms := PackedFloat64Array()
		var lat_ms := PackedFloat64Array()
		for i in n:
			var p := surface_point(w)
			var t0 := Lib.now_ms()
			op.call(p)
			call_ms.append(Lib.now_ms() - t0)
			if i < n / 3: # latency: edit call until its chunks are re-meshed (and collision rebuilt)
				var l := await lib.await_idle(w, 10000.0, 1)
				lat_ms.append(Lib.now_ms() - t0 if l >= 0 else -1.0)
		var batch := await lib.await_idle(w) # the last two thirds were issued back to back
		lib.record(op_name, {"n": n, "call_avg_ms": Lib.mean(call_ms), "call_p95_ms": Lib.percentile(call_ms, 95),
				"remesh_latency_avg_ms": Lib.mean(lat_ms), "remesh_latency_p95_ms": Lib.percentile(lat_ms, 95),
				"batch_drain_ms": batch})


func queries(w: VoxelWorld) -> void:
	var size := Vector3(BOUNDS) * 8.0
	var n := 5000 if quick else 20000
	var origins: Array[Vector3] = []
	var dirs: Array[Vector3] = []
	for i in n:
		origins.append(Vector3(rng.randf_range(0, size.x), rng.randf_range(size.y * 0.5, size.y), rng.randf_range(0, size.z)))
		dirs.append(Vector3(rng.randf_range(-1, 1), rng.randf_range(-1, -0.2), rng.randf_range(-1, 1)).normalized())
	var hits := 0
	var t0 := Lib.now_ms()
	for i in n:
		if not w.raycast(origins[i], dirs[i], 64.0).is_empty():
			hits += 1
	var ms := Lib.now_ms() - t0
	lib.record("raycast 64 m", {"n": n, "us_per_ray": ms * 1000.0 / n, "hit_ratio": float(hits) / n})

	var m := n * 10
	var pts := PackedVector3Array()
	pts.resize(m)
	for i in m:
		pts[i] = Vector3(rng.randf_range(0, size.x), rng.randf_range(0, size.y), rng.randf_range(0, size.z))
	t0 = Lib.now_ms()
	var acc := 0.0
	for i in m:
		acc += w.get_density(pts[i])
	ms = Lib.now_ms() - t0
	lib.record("get_density", {"n": m, "us_per_call": ms * 1000.0 / m})
	await process_frame


func voxelize(w: VoxelWorld) -> void:
	var size := Vector3(BOUNDS) * 8.0
	var per := PackedFloat64Array()
	for i in TREES.size():
		var at := Vector3(24 + i * 18, 0, size.z * 0.5)
		var hit := w.raycast(Vector3(at.x, size.y - 1, at.z), Vector3.DOWN, size.y)
		var xf := Transform3D(Basis().scaled(Vector3.ONE * 1.6), hit.get("position", at + Vector3.UP * 20))
		var scene: PackedScene = load(TREES[i])
		var t0 := Lib.now_ms()
		Util.plant_tree(w, scene, xf, 17, 18)
		per.append(Lib.now_ms() - t0)
	var drain := await lib.await_idle(w)
	lib.record("build_mesh (glb tree)", {"trees": per.size(), "avg_ms": Lib.mean(per), "max_ms": Lib.percentile(per, 100), "remesh_drain_ms": drain})
