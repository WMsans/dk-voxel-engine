extends SceneTree
## Engine-level checks: godot --headless --path . -s tests/run_tests.gd
## A ball must rest on meshed terrain, fall when the ground under it is dug, and stop on the new floor.
## Endless worlds must stream: load around a viewer, unload what it leaves behind, keep edits.

var failed := 0


func check(ok: bool, what: String) -> void:
	print("  %s  %s" % ["ok  " if ok else "FAIL", what])
	if not ok:
		failed += 1


func settle(frames: int) -> void:
	for i in frames:
		await physics_frame


## Deletes a save directory and everything under it (DirAccess has no recursive remove).
func rm_dir(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if not DirAccess.dir_exists_absolute(abs):
		return
	var d := DirAccess.open(path)
	if d == null:
		return
	d.list_dir_begin()
	var f := d.get_next()
	while f != "":
		if f != "." and f != "..":
			if d.current_is_dir():
				rm_dir(path.path_join(f))
			else:
				DirAccess.remove_absolute(path.path_join(f))
		f = d.get_next()
	d.list_dir_end()
	DirAccess.remove_absolute(abs)


## Waits until every chunk under `aabb` (global) is loaded and meshed at its wanted LOD; with
## `need_collision`, also until every chunk with a surface has a body.
func await_ready(w: VoxelWorld, aabb: AABB, need_collision := true, frames := 3000) -> bool:
	for i in frames:
		if w.is_area_ready(aabb, need_collision):
			return true
		await process_frame
	return false


## Waits until the world has applied everything it queued, so stats describe a settled world.
func await_idle(w: VoxelWorld, frames := 3000) -> bool:
	var quiet := 0
	for i in frames:
		await process_frame
		if w.get_pending_count() == 0:
			quiet += 1
			if quiet >= 3:
				return true
		else:
			quiet = 0
	return false


func _initialize() -> void:
	var endless := VoxelWorld.new() # not in the tree: get_world_size() must report zero, and free() keeps it out of the leak check
	check(endless.get_world_size() == Vector3.ZERO, "endless world reports zero size")
	endless.free()
	var w := VoxelWorld.new()
	w.bounds_size = Vector3i(6, 4, 6)
	w.cell_size = 0.5
	root.add_child(w)
	w.build_box(Transform3D(Basis(), Vector3(24, 6, 24)), Vector3(20, 4, 20), 3) # slab, top face at y = 10
	var cam := Camera3D.new()
	cam.position = Vector3(24, 14, 24)
	root.add_child(cam)
	cam.make_current()
	var ball := RigidBody3D.new()
	var cs := CollisionShape3D.new()
	cs.shape = SphereShape3D.new() # radius 0.5
	ball.add_child(cs)
	ball.position = Vector3(24, 12, 24)
	ball.freeze = true
	root.add_child(ball)

	check(await await_ready(w, AABB(Vector3(16, 4, 16), Vector3(16, 12, 16))), "the area under the ball is ready")
	var s := w.get_stats()
	check(s.triangles > 1000 and s.bodies > 0, "meshed: %d triangles, %d colliders, %.2f ms/chunk" % [s.triangles, s.bodies, s.mesh_ms])

	ball.freeze = false
	await settle(150)
	check(absf(ball.position.y - 10.5) < 0.05, "ball rests on the slab (y = %.3f, want 10.5)" % ball.position.y)

	var hit := w.raycast(Vector3(24.3, 20, 24.1), Vector3.DOWN, 30.0)
	check(not hit.is_empty() and absf(hit.position.y - 10.0) < 0.05 and hit.normal.y > 0.99 and hit.material == 3, "voxel raycast: %s" % hit)

	var dug := w.dig_sphere(Vector3(24, 10, 24), 3.0, 1)
	check(dug.changed and absf(dug.volume - 56.5) < 4.0, "dig removes a half sphere (%.1f m³, want 56.5)" % dug.volume)
	ball.sleeping = false
	await settle(200)
	check(absf(ball.position.y - 7.5) < 0.15, "ball falls into the crater and stops on its floor (y = %.3f, want 7.5)" % ball.position.y)

	ball.position = Vector3(24, 14, 24)
	ball.linear_velocity = Vector3.ZERO
	w.build_box(Transform3D(Basis(), Vector3(24, 9, 24)), Vector3(4, 2, 4), 7) # refill the crater, top face at y = 11
	await settle(200)
	check(absf(ball.position.y - 11.5) < 0.05, "ball lands on freshly built terrain (y = %.3f, want 11.5)" % ball.position.y)

	var mats := VoxelMaterialSet.new()
	var defs: Array[VoxelMaterialDef] = []
	for i in 4: # ids 0..3, so id 3 exists to make unbreakable
		var d := VoxelMaterialDef.new()
		d.hardness = 255 if i == 3 else 0
		defs.append(d)
	mats.materials = defs
	w.material_set = mats
	check(not w.dig_sphere(Vector3(10, 9, 10), 2.0, 5).changed, "unbreakable material resists digging")

	# A set with only entry 0 leaves every other id to entry 0's def, hardness included.
	var w2 := VoxelWorld.new()
	w2.bounds_size = Vector3i(2, 2, 2)
	w2.cell_size = 0.5
	root.add_child(w2)
	w2.build_box(Transform3D(Basis(), Vector3(2, 2, 2)), Vector3(1.5, 1.5, 1.5), 9) # unlisted id
	var single := VoxelMaterialSet.new()
	var d0 := VoxelMaterialDef.new()
	d0.hardness = 255
	var only: Array[VoxelMaterialDef] = [d0]
	single.materials = only
	w2.material_set = single
	check(not w2.dig_sphere(Vector3(2, 2, 2), 1.0, 0).changed, "unlisted material id inherits entry 0 hardness")
	w2.free()

	# Every changed edit announces the shape's global bounds.
	var edits: Array = []
	w.terrain_edited.connect(func(aabb: AABB, result: Dictionary) -> void: edits.append([aabb, result]))
	var dig_at := Vector3(30, 10, 30)
	w.dig_sphere(dig_at, 2.0, 255) # the slab is material 3, which the set above made unbreakable
	check(edits.size() == 1, "terrain_edited fires (fired %d times)" % edits.size())
	if edits.size() == 1:
		var box: AABB = edits[0][0]
		var result: Dictionary = edits[0][1]
		check(box.has_point(dig_at) and result.get("changed", false), "terrain_edited carries the edit bounds %s" % box)
	w.free()

	# A second viewer brings collision with it, even where the camera is far away.
	var w3 := VoxelWorld.new()
	w3.bounds_size = Vector3i(24, 4, 6)
	w3.cell_size = 0.5
	root.add_child(w3)
	w3.build_box(Transform3D(Basis(), Vector3(96, 6, 24)), Vector3(94, 4, 20), 3) # slab, top face at y = 10
	var cam3 := Camera3D.new()
	cam3.position = Vector3(10, 14, 24)
	root.add_child(cam3)
	cam3.make_current()
	var viewer := VoxelViewer.new()
	viewer.position = Vector3(180, 14, 24)
	viewer.collision_distance = 30.0
	root.add_child(viewer)
	var ball3 := RigidBody3D.new()
	var cs3 := CollisionShape3D.new()
	cs3.shape = SphereShape3D.new() # radius 0.5
	ball3.add_child(cs3)
	ball3.position = Vector3(180, 12, 24)
	ball3.freeze = true
	root.add_child(ball3)
	check(await await_ready(w3, AABB(Vector3(176, 8, 20), Vector3(8, 8, 8))), "the area under the second viewer's ball is ready")
	ball3.freeze = false
	await settle(150)
	check(absf(ball3.position.y - 10.5) < 0.05, "second viewer gets collision (y = %.3f, want 10.5)" % ball3.position.y)
	ball3.free()
	viewer.free()
	w3.free()
	cam3.free()

	# Endless: one viewer streams a cylinder of chunks, edits survive being unloaded and reloaded.
	var w4 := VoxelWorld.new()
	w4.cell_size = 0.5
	var gen := DemoTerrainGenerator.new()
	gen.seed = 3
	w4.generator = gen
	root.add_child(w4)
	var v := VoxelViewer.new()
	v.position = Vector3(0, 40, 0)
	root.add_child(v)
	var area := AABB(Vector3(-4, 0, -4), Vector3(8, 64, 8))
	check(await await_ready(w4, area), "endless area around the viewer is ready")
	check(await await_idle(w4), "endless world settled after the first load")
	var base := w4.get_stats()
	print("  ..  endless at x = 0: %d chunks, %d meshes, %d bodies, %d pending" % [base.chunks, base.meshes, base.bodies, base.pending])
	var g := w4.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not g.is_empty(), "endless raycast hits the ground (y = %.2f)" % g.get("position", Vector3.ZERO).y)
	var ground_y: float = g.position.y
	check(w4.dig_sphere(g.position, 3.0, 255).changed, "endless crater dug")

	v.position = Vector3(600, 40, 0)
	var unloaded := false
	for i in 900:
		await process_frame
		if w4.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0).is_empty():
			unloaded = true
			break
	check(unloaded, "far chunks unload when the viewer moves")
	check(await await_ready(w4, AABB(Vector3(596, 0, -4), Vector3(8, 64, 8))), "endless area at x = 600 is ready")
	check(await await_idle(w4), "endless world settled after the move")
	var moved := w4.get_stats()
	print("  ..  endless at x = 600: %d chunks, %d meshes, %d bodies, %d pending" % [moved.chunks, moved.meshes, moved.bodies, moved.pending])
	check(moved.meshes <= base.meshes * 1.2 and moved.bodies <= base.bodies * 1.2,
			"teleport frees far chunks (meshes %d -> %d, bodies %d -> %d)" % [base.meshes, moved.meshes, base.bodies, moved.bodies])

	v.position = Vector3(0, 40, 0)
	check(await await_ready(w4, area), "endless area reloads at the old viewer position")
	var back := w4.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back.is_empty() and back.position.y < ground_y - 2.5, "endless crater survives unload (y %.2f -> %.2f)" % [ground_y, back.get("position", Vector3.ZERO).y])

	# A restored chunk stays authoritative: a second unload/reload must keep the crater too.
	v.position = Vector3(600, 40, 0)
	for i in 900:
		await process_frame
		if w4.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0).is_empty():
			break
	v.position = Vector3(0, 40, 0)
	check(await await_ready(w4, area), "endless area reloads a second time")
	var back2 := w4.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back2.is_empty() and back2.position.y < ground_y - 2.5, "endless crater survives two unload/reload round trips (y %.2f -> %.2f)" % [ground_y, back2.get("position", Vector3.ZERO).y])

	# reset() twice while workers are busy: no stale result may reach the new world.
	var busy := 0
	for i in 2:
		w4.reset()
		await process_frame
		busy = maxi(busy, w4.get_pending_count())
	check(busy > 0, "reset while busy had work in flight (%d)" % busy)
	check(await await_ready(w4, area), "endless area is ready after two resets")
	w4.free()
	v.free()

	# Collision bodies only exist at LOD0, so a viewer whose collision distance reaches past LOD1 must
	# not keep re-queueing LOD1 chunks for a body that can never be built.
	var w5 := VoxelWorld.new()
	w5.cell_size = 0.5
	var gen5 := DemoTerrainGenerator.new()
	gen5.seed = 5
	w5.generator = gen5
	root.add_child(w5)
	var v5 := VoxelViewer.new()
	v5.position = Vector3(0, 40, 0)
	v5.collision_distance = 400.0
	root.add_child(v5)
	check(await await_ready(w5, AABB(Vector3(-4, 0, -4), Vector3(8, 64, 8))), "wide collision area around the viewer is ready")
	check(await await_idle(w5), "wide collision world settled")
	w5.set_lod1_distance(w5.get_lod1_distance()) # a no-op property set still forces a rescan
	await process_frame
	await process_frame
	check(w5.get_pending_count() == 0, "collision clamp: a rescan leaves far chunks alone (%d pending)" % w5.get_pending_count())
	w5.free()
	v5.free()

	# A rotated world node must transform the whole AABB, not just two opposite corners.
	var w6 := VoxelWorld.new()
	w6.cell_size = 0.5
	w6.rotation_degrees.y = 30.0
	var gen6 := DemoTerrainGenerator.new()
	gen6.seed = 6
	w6.generator = gen6
	root.add_child(w6)
	var v6 := VoxelViewer.new()
	v6.position = Vector3(0, 40, 0) # at the world origin, so this is its local position too
	v6.view_distance = 160.0
	v6.vertical_view_distance = 96.0
	root.add_child(v6)
	check(await await_ready(w6, AABB(Vector3(-4, 0, -4), Vector3(8, 64, 8))), "rotated world area around the viewer is ready")
	check(await await_idle(w6), "rotated world settled")
	# This global square's local corners sit ~193 m from the viewer (outside the 160 m view distance),
	# while the two-corner diagonal stays inside: all 8 corners must be transformed.
	check(not w6.is_area_ready(AABB(Vector3(-100, 0, -100), Vector3(200, 64, 200)), false), "rotated world transforms all 8 corners of the area")
	w6.free()
	v6.free()

	# Saving: save_directory writes edited chunks, so a new world on the same directory loads the crater.
	var save_dir := "user://dkv_test_save"
	rm_dir(save_dir)
	var s7 := VoxelWorld.new()
	s7.cell_size = 0.5
	var gen7 := DemoTerrainGenerator.new()
	gen7.seed = 3
	s7.generator = gen7
	s7.save_directory = save_dir
	root.add_child(s7)
	var v7 := VoxelViewer.new()
	v7.position = Vector3(0, 40, 0)
	root.add_child(v7)
	check(await await_ready(s7, area), "save: endless area ready")
	check(await await_idle(s7), "save: endless world settled")
	await settle(100) # earlier worlds (all freed with queue_free) must be gone
	var g7 := s7.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	var ground7: float = g7.position.y
	check(s7.dig_sphere(g7.position, 3.0, 255).changed, "save: endless crater dug")
	check(s7.save() == OK, "save() returns OK")
	s7.free()
	v7.free()

	var s8 := VoxelWorld.new()
	s8.cell_size = 0.5
	var gen8 := DemoTerrainGenerator.new()
	gen8.seed = 3
	s8.generator = gen8
	s8.save_directory = save_dir
	root.add_child(s8)
	var v8 := VoxelViewer.new()
	v8.position = Vector3(0, 40, 0)
	root.add_child(v8)
	check(await await_ready(s8, area), "save round trip: area ready in the new world")
	var back8 := s8.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back8.is_empty() and back8.position.y < ground7 - 2.5, "save round trip restores the crater (y %.2f -> %.2f)" % [ground7, back8.get("position", Vector3.ZERO).y])
	s8.free()
	v8.free()

	# Freeing a world flushes its edited chunks even without an explicit save().
	rm_dir(save_dir)
	var s9 := VoxelWorld.new()
	s9.cell_size = 0.5
	var gen9 := DemoTerrainGenerator.new()
	gen9.seed = 3
	s9.generator = gen9
	s9.save_directory = save_dir
	root.add_child(s9)
	var v9 := VoxelViewer.new()
	v9.position = Vector3(0, 40, 0)
	root.add_child(v9)
	check(await await_ready(s9, area), "free without save: endless area ready")
	check(await await_idle(s9), "free without save: endless world settled")
	var g9 := s9.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	var ground9: float = g9.position.y
	s9.dig_sphere(g9.position, 3.0, 255)
	s9.queue_free()
	await process_frame
	await process_frame

	var s10 := VoxelWorld.new()
	s10.cell_size = 0.5
	var gen10 := DemoTerrainGenerator.new()
	gen10.seed = 3
	s10.generator = gen10
	s10.save_directory = save_dir
	root.add_child(s10)
	var v10 := VoxelViewer.new()
	v10.position = Vector3(0, 40, 0)
	root.add_child(v10)
	check(await await_ready(s10, area), "free without save: area ready in the new world")
	var back10 := s10.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back10.is_empty() and back10.position.y < ground9 - 2.5, "free without save keeps edits (y %.2f -> %.2f)" % [ground9, back10.get("position", Vector3.ZERO).y])
	s10.free()
	v10.free()
	v9.free()

	# A save written with another cell size is rejected, not loaded (the error text is expected).
	var s11 := VoxelWorld.new()
	s11.cell_size = 1.0
	var gen11 := DemoTerrainGenerator.new()
	gen11.seed = 3
	s11.generator = gen11
	s11.save_directory = save_dir
	root.add_child(s11)
	var v11 := VoxelViewer.new()
	v11.position = Vector3(0, 40, 0)
	root.add_child(v11)
	check(await await_ready(s11, area), "cell size mismatch: area ready")
	var back11 := s11.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	print("  ..  cell size 1.0 ground y = %.2f (was %.2f)" % [back11.get("position", Vector3.ZERO).y, ground9])
	check(not back11.is_empty() and absf(back11.position.y - ground9) < 1.0, "cell size mismatch rejected (y %.2f, was %.2f)" % [back11.get("position", Vector3.ZERO).y, ground9])
	s11.free()
	v11.free()

	# A bounded world saves its edited chunks and rebuilds from the same directory.
	var bsave := "user://dkv_test_save_bounded"
	rm_dir(bsave)
	var b1 := VoxelWorld.new()
	b1.bounds_size = Vector3i(6, 4, 6)
	b1.cell_size = 0.5
	b1.save_directory = bsave
	root.add_child(b1)
	b1.build_box(Transform3D(Basis(), Vector3(24, 6, 24)), Vector3(20, 4, 20), 3) # slab, top face at y = 10
	var digc := Vector3(24, 10, 24)
	b1.dig_sphere(digc, 3.0, 255)
	check(b1.save() == OK, "bounded save() returns OK")
	b1.free()

	var b2 := VoxelWorld.new()
	b2.bounds_size = Vector3i(6, 4, 6)
	b2.cell_size = 0.5
	b2.save_directory = bsave
	root.add_child(b2)
	# The saved chunks are the whole edited slab, so the new world has it back without rebuilding it.
	var top := b2.raycast(Vector3(10, 20, 10), Vector3.DOWN, 30.0) # outside the crater
	check(not top.is_empty() and absf(top.position.y - 10.0) < 0.05, "bounded world loads its saved slab (top y = %.2f)" % top.get("position", Vector3.ZERO).y)
	var replay := b2.dig_sphere(digc, 3.0, 255)
	check(absf(replay.volume) < 1.0, "bounded world loads its save (replayed dig removes %.2f m³)" % replay.volume)
	var floor := b2.raycast(digc + Vector3(0, 10, 0), Vector3.DOWN, 30.0)
	check(not floor.is_empty() and absf(floor.position.y - 7.0) < 0.25, "bounded world loads the crater floor (y = %.2f)" % floor.get("position", Vector3.ZERO).y)
	b2.free()

	# A store entry must not shadow or overwrite a newer file: save after moving away and back.
	var s12_dir := "user://dkv_test_save_reload"
	rm_dir(s12_dir)
	var s12 := VoxelWorld.new()
	s12.cell_size = 0.5
	var gen12 := DemoTerrainGenerator.new()
	gen12.seed = 3
	s12.generator = gen12
	s12.save_directory = ""
	root.add_child(s12)
	var v12 := VoxelViewer.new()
	v12.position = Vector3(0, 40, 0)
	root.add_child(v12)
	check(await await_ready(s12, area), "reload: endless area ready")
	check(await await_idle(s12), "reload: endless world settled")
	var g12 := s12.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	var ground12: float = g12.position.y
	check(s12.dig_sphere(g12.position, 3.0, 255).changed, "reload: endless crater dug")
	v12.position = Vector3(600, 40, 0) # the edited chunk unloads into the memory store
	for i in 900:
		await process_frame
		if s12.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0).is_empty():
			break
	s12.save_directory = s12_dir
	check(s12.save() == OK, "reload: store written after the move away")
	v12.position = Vector3(0, 40, 0) # loads the file; the stale store entry must not shadow it
	check(await await_ready(s12, area), "reload: endless area reloads")
	check(s12.dig_sphere(Vector3(0, 12.0, 0), 2.0, 255).changed, "reload: crater dug deeper")
	v12.position = Vector3(600, 40, 0)
	for i in 900:
		await process_frame
		if s12.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0).is_empty():
			break
	v12.position = Vector3(0, 40, 0)
	check(await await_ready(s12, area), "reload: endless area reloads after the second move")
	check(s12.save() == OK, "reload: save after the second move")
	s12.free()
	v12.free()

	var s13 := VoxelWorld.new()
	s13.cell_size = 0.5
	var gen13 := DemoTerrainGenerator.new()
	gen13.seed = 3
	s13.generator = gen13
	s13.save_directory = s12_dir
	root.add_child(s13)
	var v13 := VoxelViewer.new()
	v13.position = Vector3(0, 40, 0)
	root.add_child(v13)
	check(await await_ready(s13, area), "reload: area ready in the new world")
	var back13 := s13.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back13.is_empty() and back13.position.y < ground12 - 3.5, "store entry does not overwrite a newer file (y %.2f -> %.2f)" % [ground12, back13.get("position", Vector3.ZERO).y])
	s13.free()
	v13.free()

	# A file whose body is shorter than its header says must be reported and fall back to the generator.
	var short_dir := "user://dkv_test_save_short"
	rm_dir(short_dir)
	var s14 := VoxelWorld.new()
	s14.cell_size = 0.5
	var gen14 := DemoTerrainGenerator.new()
	gen14.seed = 3
	s14.generator = gen14
	s14.save_directory = short_dir
	root.add_child(s14)
	var v14 := VoxelViewer.new()
	v14.position = Vector3(0, 40, 0)
	root.add_child(v14)
	check(await await_ready(s14, area), "short body: endless area ready")
	var g14 := s14.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	var ground14: float = g14.position.y
	check(s14.dig_sphere(g14.position, 3.0, 255).changed, "short body: crater dug")
	check(s14.save() == OK, "short body: save() returns OK")
	var victim_path := short_dir.path_join("0_1_0.dkvc") # the chunk the dig centre sits in
	var victim_file := FileAccess.open_compressed(victim_path, FileAccess.READ, FileAccess.COMPRESSION_ZSTD)
	check(victim_file != null, "short body: save file exists (%s)" % victim_path)
	if victim_file != null:
		var magic := victim_file.get_32()
		var version := victim_file.get_16()
		var cell := victim_file.get_float()
		var gen_name := victim_file.get_pascal_string()
		var body := victim_file.get_buffer(victim_file.get_32())
		victim_file = null
		# The header now claims a body longer than the stream holds: the reader sees a short read.
		var corrupt := FileAccess.open_compressed(victim_path, FileAccess.WRITE, FileAccess.COMPRESSION_ZSTD)
		corrupt.store_32(magic)
		corrupt.store_16(version)
		corrupt.store_float(cell)
		corrupt.store_pascal_string(gen_name)
		corrupt.store_32(body.size() + 4096)
		corrupt.store_buffer(body)
		corrupt = null
	s14.free()
	v14.free()

	var s15 := VoxelWorld.new()
	s15.cell_size = 0.5
	var gen15 := DemoTerrainGenerator.new()
	gen15.seed = 3
	s15.generator = gen15
	s15.save_directory = short_dir
	root.add_child(s15)
	var v15 := VoxelViewer.new()
	v15.position = Vector3(0, 40, 0)
	root.add_child(v15)
	check(await await_ready(s15, area), "short body: area ready in the new world")
	var back15 := s15.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back15.is_empty() and absf(back15.position.y - ground14) < 0.05, "short body falls back to generated terrain (y %.2f, was %.2f)" % [back15.get("position", Vector3.ZERO).y, ground14])
	s15.free()
	v15.free()

	print("ALL GODOT TESTS PASSED" if failed == 0 else "FAILED (%d)" % failed)
	quit(1 if failed else 0)