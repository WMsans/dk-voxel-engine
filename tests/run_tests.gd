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

	print("ALL GODOT TESTS PASSED" if failed == 0 else "FAILED (%d)" % failed)
	quit(1 if failed else 0)