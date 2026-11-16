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

	# The material set must reach the shader: 32 slots of colour and pattern parameters, entry 0 carrying
	# the def's values. This is the only automated check that the uniforms are wired at all.
	var shader: Shader = load("res://addons/dk_voxel/voxel_terrain.gdshader")
	check(shader != null, "terrain shader loads")
	var sm := ShaderMaterial.new()
	sm.shader = shader
	var shader_def := VoxelMaterialDef.new()
	shader_def.color_a = Color(0.25, 0.5, 0.75)
	shader_def.roughness = 0.42
	var shader_set := VoxelMaterialSet.new()
	var shader_defs: Array[VoxelMaterialDef] = [shader_def]
	shader_set.materials = shader_defs
	var mw := VoxelWorld.new()
	mw.material = sm
	mw.material_set = shader_set
	var color_a: PackedColorArray = sm.get_shader_parameter("color_a")
	var color_b: PackedColorArray = sm.get_shader_parameter("color_b")
	var params: Array = sm.get_shader_parameter("params")
	check(color_a.size() == 32 and color_b.size() == 32 and params.size() == 32,
			"material uniforms fill 32 slots (color_a %d, color_b %d, params %d)" % [color_a.size(), color_b.size(), params.size()])
	if color_a.size() == 32 and params.size() == 32:
		var p0: Vector4 = params[0]
		check(color_a[0].is_equal_approx(Color(0.25, 0.5, 0.75)) and is_equal_approx(p0.x, 0.3) and is_equal_approx(p0.y, 0.42),
				"material set reaches the shader (color_a %s, params %s)" % [color_a[0], p0])
	mw.free()

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
	var rough_packed: Texture2DArray = shader_mat.get_shader_parameter("roughness_tex")
	var ao_packed: Texture2DArray = shader_mat.get_shader_parameter("ao_tex")
	check(rough_packed != null and rough_packed.get_layers() == 32 and ao_packed != null and ao_packed.get_layers() == 32,
			"roughness and AO pack 32 layers each (%s, %s)" % [rough_packed, ao_packed])
	# get_layer_data() needs a renderer that keeps image data; --headless uses the dummy driver, which does
	# not. When it is readable the layer's pixels are checked; when it is not, the array's size and identity
	# still prove the packing ran. Task 7's --shot run is the visual proof of the pixels.
	var can_read := packed != null and packed.get_layer_data(0) != null
	print("  ..  array layers %s" % ["readable" if can_read else "unreadable (dummy renderer)"])
	if can_read:
		var layer0 := packed.get_layer_data(0)
		check(layer0.get_pixel(0, 0).r > 0.9 and layer0.get_pixel(0, 0).g < 0.1, "layer 0 is the def's own texture")
		# Unset maps must be white, so the scalar roughness and the baked voxel AO keep the whole say.
		check(rough_packed.get_layer_data(2).get_pixel(0, 0).r > 0.99 and ao_packed.get_layer_data(2).get_pixel(0, 0).r > 0.99,
				"a def without maps packs flat 1.0 roughness and AO")
	else:
		check(packed.get_width() == 256 and packed.get_height() == 256, "the arrays are packed at the common size (%dx%d)" % [packed.get_width(), packed.get_height()])

	# Swapping the texture in place repacks, and so does replacing the set's materials array. A repack gives a
	# new array object, which is observable even when the layer data is not.
	var packed_before := packed
	var green_img := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	green_img.fill(Color(0.0, 1.0, 0.0))
	def0.albedo = ImageTexture.create_from_image(green_img)
	packed = shader_mat.get_shader_parameter("albedo_tex")
	if can_read:
		check(packed.get_layer_data(0).get_pixel(0, 0).g > 0.9, "editing a def's texture reaches the shader")
	else:
		check(packed != null and packed != packed_before, "editing a def's texture repacks the arrays")
	packed_before = packed
	var rough_before := rough_packed
	def0.roughness_map = ImageTexture.create_from_image(green_img)
	check(shader_mat.get_shader_parameter("roughness_tex") != rough_before, "editing a def's roughness map repacks the arrays")
	var def0b := VoxelMaterialDef.new()
	def0b.albedo = albedo_tex
	var swapped: Array[VoxelMaterialDef] = [def0b]
	tex_set.materials = swapped
	packed = shader_mat.get_shader_parameter("albedo_tex")
	if can_read:
		check(packed.get_layer_data(0).get_pixel(0, 0).r > 0.9, "replacing materials repacks")
	else:
		check(packed != null and packed != packed_before, "replacing materials repacks")

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
	# A set first, then a material: this ShaderMaterial starts with nothing packed, so a pack that is skipped
	# leaves albedo_tex unset and this fails.
	var odd_world := VoxelWorld.new()
	var odd_mat := ShaderMaterial.new()
	odd_mat.shader = shader
	odd_world.material_set = odd_set
	odd_world.material = odd_mat
	var odd_packed: Texture2DArray = odd_mat.get_shader_parameter("albedo_tex")
	check(odd_packed != null and odd_packed.get_layers() == 32 and odd_packed.get_width() == 256,
			"mixed sizes and an over-long set pack 32 layers at the common size (%s)" % [odd_packed])

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

	# A save failure during reset() must not throw edits away: they stay in the memory store and a later
	# save() to a writable directory writes them. save_directory sits under a regular file, so every write
	# fails (the error text is expected).
	var blocker_path := "user://dkv_test_not_a_dir"
	var blocker := FileAccess.open(blocker_path, FileAccess.WRITE)
	check(blocker != null, "failed reset save: created the blocking file")
	blocker.store_string("not a directory")
	blocker = null
	var failed_dir := "user://dkv_test_save_failed_reset"
	rm_dir(failed_dir)
	var s16 := VoxelWorld.new()
	s16.cell_size = 0.5
	var gen16 := DemoTerrainGenerator.new()
	gen16.seed = 3
	s16.generator = gen16
	s16.save_directory = blocker_path.path_join("sub")
	root.add_child(s16)
	var v16 := VoxelViewer.new()
	v16.position = Vector3(0, 40, 0)
	root.add_child(v16)
	check(await await_ready(s16, area), "failed reset save: endless area ready")
	check(await await_idle(s16), "failed reset save: endless world settled")
	var g16 := s16.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	var ground16: float = g16.position.y
	check(s16.dig_sphere(g16.position, 3.0, 255).changed, "failed reset save: crater dug")
	s16.reset() # the flush fails; the edit must survive in the store
	s16.save_directory = failed_dir
	check(s16.save() == OK, "failed reset save: a later save() succeeds")
	s16.free()
	v16.free()

	var s17 := VoxelWorld.new()
	s17.cell_size = 0.5
	var gen17 := DemoTerrainGenerator.new()
	gen17.seed = 3
	s17.generator = gen17
	s17.save_directory = failed_dir
	root.add_child(s17)
	var v17 := VoxelViewer.new()
	v17.position = Vector3(0, 40, 0)
	root.add_child(v17)
	check(await await_ready(s17, area), "failed reset save: area ready in the new world")
	var back17 := s17.raycast(Vector3(0, 63, 0), Vector3.DOWN, 80.0)
	check(not back17.is_empty() and back17.position.y < ground16 - 2.5, "failed reset save keeps the edit (y %.2f -> %.2f)" % [ground16, back17.get("position", Vector3.ZERO).y])
	s17.free()
	v17.free()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(blocker_path))
	rm_dir(failed_dir)

	# The level profile: the Blender-made demo level, bounded and generated up front.
	var lg := LevelTerrainGenerator.new()
	lg.level_path = "res://demo/levels/mini_level.dkvl"
	var size: Vector3 = lg.get_level_size()
	check(size.x > 10.0 and size.y > 10.0, "level profile: size read from the file (%s)" % size)
	var lw := VoxelWorld.new()
	lw.cell_size = 0.5
	lw.bounds_size = Vector3i((size / 8.0).ceil())
	lw.generator = lg
	var sp: Vector3 = lg.get_spawn_point()
	var lhit := lw.raycast(Vector3(sp.x, size.y, sp.z), Vector3.DOWN, size.y)
	check(not lhit.is_empty() and lhit.position.y < sp.y and lhit.material == 1, "level profile: grass under the spawn point (%s)" % lhit)
	check(lw.get_density(Vector3(1, 1, 1)) < 1.0, "level profile: outside every part is air")
	lw.free()

	print("ALL GODOT TESTS PASSED" if failed == 0 else "FAILED (%d)" % failed)
	quit(1 if failed else 0)
