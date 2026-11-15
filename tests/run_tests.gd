extends SceneTree
## Engine-level checks: godot --headless --path . -s tests/run_tests.gd
## A ball must rest on meshed terrain, fall when the ground under it is dug, and stop on the new floor.

var failed := 0


func check(ok: bool, what: String) -> void:
	print("  %s  %s" % ["ok  " if ok else "FAIL", what])
	if not ok:
		failed += 1


func settle(frames: int) -> void:
	for i in frames:
		await physics_frame


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

	for i in 600:
		await process_frame
		if w.get_pending_count() == 0 and w.get_stats().bodies > 0:
			break
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

	print("ALL GODOT TESTS PASSED" if failed == 0 else "FAILED (%d)" % failed)
	quit(1 if failed else 0)
