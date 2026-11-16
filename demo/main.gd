extends Node3D
## Demo for the dk_voxel engine: a destructible dual-contoured world.
##   WASD move, Space jump, Shift sprint, V toggle fly
##   LMB dig   RMB build   Q smooth   E place a sharp block   F throw a bomb
##   Wheel brush size   1-5 build material   R new world   P switch profile   Esc free the mouse
## Command line (after --):  --shot=<png>  render a beauty shot and quit
##                           --profile=procedural|level  which terrain generation profile to start with
## Profiles: "procedural" = DemoTerrainGenerator, endless; "level" = LevelTerrainGenerator, the hand-made
## Blender level in demo/levels (Bananza-style: bounded, every part authored).

const MATERIALS := [ # name, colour a, colour b, pattern scale, roughness, metallic, emission, hardness
	["Default", Color(0.5, 0.5, 0.5), Color(0.6, 0.6, 0.6), 0.5, 0.9, 0.0, 0.0, 0],
	["Grass", Color(0.13, 0.36, 0.09), Color(0.34, 0.52, 0.13), 0.22, 0.95, 0.0, 0.0, 0],
	["Soil", Color(0.27, 0.15, 0.08), Color(0.44, 0.28, 0.15), 0.3, 0.95, 0.0, 0.0, 0],
	["Rock", Color(0.27, 0.29, 0.36), Color(0.50, 0.50, 0.54), 0.16, 0.8, 0.0, 0.0, 1],
	["Sand", Color(0.66, 0.50, 0.28), Color(0.80, 0.68, 0.44), 0.4, 1.0, 0.0, 0.0, 0],
	["Gold", Color(0.95, 0.55, 0.06), Color(1.0, 0.80, 0.28), 0.35, 0.3, 1.0, 0.2, 1],
	["Bedrock", Color(0.08, 0.06, 0.11), Color(0.18, 0.14, 0.23), 0.14, 0.6, 0.0, 0.0, 255],
	["Brick", Color(0.46, 0.20, 0.14), Color(0.62, 0.36, 0.26), 0.3, 0.85, 0.0, 0.0, 1],
	["Crystal", Color(0.08, 0.60, 0.80), Color(0.60, 0.28, 0.85), 0.2, 0.15, 0.3, 1.2, 2],
]
const BUILD_KEYS := [7, 2, 3, 4, 8]
const CELL := 0.5
const GROUND_FROM := 400.0 # raycast down from here to find the ground: endless worlds have no height

var world: VoxelWorld
const LEVEL_PATH := "res://demo/levels/mini_level.dkvl"
var procedural := DemoTerrainGenerator.new()
var level := LevelTerrainGenerator.new()
var profile := "level"
var materials: VoxelMaterialSet
var player: CharacterBody3D
var cam: Camera3D
var hud: Label
var brush := 1.6
var build_mat := 7
var fly := false
var gold := 0.0
var cooldown := 0.0
var spawned := false
var spawn_token := 0
var shot_path := ""
var debris: Array[RigidBody3D] = []
var debris_mats := {}
var box_mesh := BoxMesh.new()
var box_shape := BoxShape3D.new()


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--shot="):
			shot_path = a.trim_prefix("--shot=")
		elif a.begins_with("--profile="):
			profile = a.trim_prefix("--profile=")
	level.level_path = LEVEL_PATH
	_make_environment()
	materials = _make_material_set()
	world = VoxelWorld.new()
	world.cell_size = CELL
	world.material_set = materials
	world.material = _make_terrain_material()
	add_child(world)
	_make_player()
	_make_hud()
	_new_world(7)
	if shot_path == "":
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


func _new_world(world_seed: int) -> void:
	procedural.seed = world_seed
	if profile == "level":
		world.generator = level
		world.bounds_size = Vector3i((level.get_level_size() / (16.0 * CELL)).ceil()) # bounded, like a Bananza VoxelField
	else:
		world.generator = procedural
		world.bounds_size = Vector3i.ZERO # endless: chunks stream around the player's viewer
	world.reset()
	spawned = false
	spawn_token += 1 # a quick second R must not leave the old spawn building ruins
	_spawn(spawn_token)


## Puts the player on the ground at the origin, then builds the ruins, once the area is streamed in.
func _spawn(token: int) -> void:
	var at := level.get_spawn_point() if profile == "level" else Vector3.ZERO
	# Move the player (and its viewer) to the spawn point before waiting: the origin is outside every
	# viewer's distance after flying away, so waiting there would time out.
	player.position = at + Vector3.UP * 20.0
	player.velocity = Vector3.ZERO
	await _wait_ready(AABB(at + Vector3(-32, 0, -32), Vector3(64, 64, 64)), false) # ground and ruins
	if token != spawn_token:
		return
	player.position = _ground(at) + Vector3.UP * 3.0
	player.velocity = Vector3.ZERO
	if profile == "procedural":
		_build_ruins(at + Vector3(14.0, 0, -6.0))
	await _wait_ready(AABB(player.position + Vector3(-8, -2, -8), Vector3(16, 12, 16)), true) # under the player
	if token != spawn_token:
		return
	spawned = true


## Waits (bounded, so a stalled stream cannot hang the demo) until the chunks under `aabb` are meshed.
func _wait_ready(aabb: AABB, need_collision: bool, frames := 600) -> void:
	for i in frames:
		if world.is_area_ready(aabb, need_collision):
			return
		await get_tree().process_frame


func _make_material_set() -> VoxelMaterialSet:
	var set := VoxelMaterialSet.new()
	var defs: Array[VoxelMaterialDef] = []
	for m in MATERIALS:
		var d := VoxelMaterialDef.new()
		d.name = m[0]
		d.color_a = m[1]
		d.color_b = m[2]
		d.pattern_scale = m[3]
		d.roughness = m[4]
		d.metallic = m[5]
		d.emission = m[6]
		d.hardness = m[7]
		defs.append(d)
	# Two real texture files on rock: this is the whole game-side job. The colours above tint the texture, so
	# rock's are lightened to near-white here to let the image show.
	var rock := defs[3]
	rock.albedo = load("res://demo/textures/rock_albedo.png")
	rock.normal = load("res://demo/textures/rock_normal.png")
	rock.color_a = Color(0.87, 0.86, 0.88)
	rock.color_b = Color(1.0, 0.98, 0.95)
	rock.pattern_scale = 0.35 # tiles per metre: 8 m chunks x 0.35 is not a whole number, so a chunk-local
	# UV space would show a seam every 8 m — this value keeps that regression visible
	set.materials = defs
	return set


func _material(id: int) -> VoxelMaterialDef:
	if id < 0 or id >= materials.materials.size():
		id = 0
	return materials.materials[id]


## Sharp-edged structures: the Hermite path of the mesher. Everything here is plain CSG on the voxel field.
func _build_ruins(at: Vector3) -> void:
	var g := _ground(at)
	var base := Transform3D(Basis(Vector3.UP, 0.4), g + Vector3.UP * 3.0)
	world.build_box(base, Vector3(5, 4.5, 5), 7)
	world.dig_box(base.translated_local(Vector3(0, 0.5, 0)), Vector3(4, 4.0, 4), 9)
	world.dig_box(base.translated_local(Vector3(0, -1.0, 5)), Vector3(1.2, 2.2, 1.5), 9) # door
	world.dig_cylinder(base.translated_local(Vector3(5, 1.0, 0)) * Transform3D(Basis(Vector3.FORWARD, PI * 0.5), Vector3.ZERO), 1.4, 1.5, 9) # round window
	for i in 5: # colonnade
		var p := _ground(at + Vector3(-10.0 - i * 4.0, 0, 9.0))
		world.build_cylinder(Transform3D(Basis(), p + Vector3.UP * 3.0), 0.8, 4.0, 3)
		world.build_box(Transform3D(Basis(), p + Vector3.UP * 7.3), Vector3(1.3, 0.35, 1.3), 3)
	for i in 8: # stairs up to the tower
		world.build_box(base.translated_local(Vector3(0, -4.2 + i * 0.5, 13.0 - i * 1.0)), Vector3(1.6, 0.25, 0.5), 3)


func _ground(at: Vector3) -> Vector3:
	var hit := world.raycast(Vector3(at.x, GROUND_FROM, at.z), Vector3.DOWN, GROUND_FROM + 200.0)
	return hit.get("position", Vector3(at.x, 20.0, at.z))


func _make_terrain_material() -> ShaderMaterial:
	# Colours, textures and parameters all come from the material set; the shader needs no inputs of its own.
	var mat := ShaderMaterial.new()
	mat.shader = load("res://addons/dk_voxel/voxel_terrain.gdshader")
	return mat


func _make_environment() -> void:
	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.16, 0.36, 0.78)
	sky_mat.sky_horizon_color = Color(0.72, 0.82, 0.92)
	sky_mat.ground_bottom_color = Color(0.55, 0.66, 0.78)
	sky_mat.ground_horizon_color = Color(0.72, 0.82, 0.92)
	var sky := Sky.new()
	sky.sky_material = sky_mat
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_energy = 0.7
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.ssao_enabled = true
	env.ssao_radius = 1.6
	env.ssao_intensity = 2.2
	env.ssil_enabled = true
	env.glow_enabled = true
	env.glow_intensity = 0.6
	env.glow_bloom = 0.05
	env.fog_enabled = true
	env.fog_light_color = Color(0.70, 0.80, 0.92)
	env.fog_density = 0.0028
	env.fog_aerial_perspective = 0.6
	env.fog_sky_affect = 0.0
	env.adjustment_enabled = true
	env.adjustment_saturation = 1.25
	env.adjustment_contrast = 1.08
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-42, -35, 0)
	sun.light_color = Color(1.0, 0.94, 0.82)
	sun.light_energy = 1.3
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 160.0
	sun.shadow_bias = 0.04
	sun.shadow_normal_bias = 1.4
	add_child(sun)


func _make_player() -> void:
	player = CharacterBody3D.new()
	var col := CollisionShape3D.new()
	var cap := CapsuleShape3D.new()
	cap.radius = 0.4
	cap.height = 1.7
	col.shape = cap
	player.add_child(col)
	player.floor_max_angle = deg_to_rad(50)
	player.floor_snap_length = 0.35
	cam = Camera3D.new()
	cam.position.y = 0.65
	cam.fov = 80
	cam.far = 600
	player.add_child(cam)
	var lamp := OmniLight3D.new() # so caves are not pitch black
	lamp.omni_range = 14
	lamp.light_energy = 0.7
	lamp.light_color = Color(1.0, 0.85, 0.65)
	cam.add_child(lamp)
	var vv := VoxelViewer.new() # streams and collides around the player, not around the camera
	vv.position = Vector3(0, 0.9, 0)
	player.add_child(vv)
	add_child(player)
	cam.make_current()


func _make_hud() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	hud = Label.new()
	hud.position = Vector2(12, 8)
	hud.add_theme_color_override("font_outline_color", Color.BLACK)
	hud.add_theme_constant_override("outline_size", 5)
	layer.add_child(hud)
	var cross := Label.new()
	cross.text = "+"
	cross.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	cross.add_theme_color_override("font_outline_color", Color.BLACK)
	cross.add_theme_constant_override("outline_size", 4)
	layer.add_child(cross)
	if shot_path != "":
		layer.visible = false


func _unhandled_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		player.rotate_y(-e.relative.x * 0.0025)
		cam.rotation.x = clampf(cam.rotation.x - e.relative.y * 0.0025, -1.5, 1.5)
	elif e is InputEventMouseButton and e.pressed:
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		elif e.button_index == MOUSE_BUTTON_WHEEL_UP:
			brush = minf(brush + 0.2, 5.0)
		elif e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			brush = maxf(brush - 0.2, 0.6)
	elif e is InputEventKey and e.pressed and not e.echo:
		match e.keycode:
			KEY_ESCAPE: Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
			KEY_V: fly = not fly
			KEY_R: _new_world(randi())
			KEY_P:
				profile = "level" if profile == "procedural" else "procedural"
				_new_world(procedural.seed)
			KEY_F: _throw_bomb()
			KEY_E: _place_block()
			KEY_1, KEY_2, KEY_3, KEY_4, KEY_5: build_mat = BUILD_KEYS[e.keycode - KEY_1]


func _physics_process(dt: float) -> void:
	# Hold the player until the ground under them has a collision body.
	if not spawned:
		return
	var dir := Input.get_vector(&"ui_left", &"ui_right", &"ui_up", &"ui_down")
	dir += Vector2(float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)), float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
	var speed := 14.0 if Input.is_physical_key_pressed(KEY_SHIFT) else 7.0
	if fly:
		var v := cam.global_basis * Vector3(dir.x, 0, dir.y) * speed * 2.0
		v.y += (float(Input.is_physical_key_pressed(KEY_SPACE)) - float(Input.is_physical_key_pressed(KEY_CTRL))) * speed * 2.0
		player.global_position += v * dt
		player.velocity = Vector3.ZERO
	else:
		var wish := player.global_basis * Vector3(dir.x, 0, dir.y).limit_length() * speed
		player.velocity.x = move_toward(player.velocity.x, wish.x, 60.0 * dt)
		player.velocity.z = move_toward(player.velocity.z, wish.z, 60.0 * dt)
		player.velocity.y -= 24.0 * dt
		if player.is_on_floor() and Input.is_physical_key_pressed(KEY_SPACE):
			player.velocity.y = 9.0
		player.move_and_slide()
		if player.position.y < -20.0: # fell out of the world
			player.position = _ground(level.get_spawn_point() if profile == "level" else player.position) + Vector3.UP * 2.0
			player.velocity = Vector3.ZERO

	cooldown -= dt
	if cooldown <= 0.0 and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		var lmb := Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
		var rmb := Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
		var smooth := Input.is_physical_key_pressed(KEY_Q)
		if lmb or rmb or smooth:
			var hit := world.raycast(cam.global_position, -cam.global_basis.z, 14.0)
			if not hit.is_empty():
				cooldown = 0.07
				var p: Vector3 = hit.position
				if lmb:
					_dug(world.dig_sphere(p - hit.normal * brush * 0.3, brush, 1), p, 4.0)
				elif rmb and p.distance_to(cam.global_position) > brush + 1.2:
					world.build_sphere(p + hit.normal * brush * 0.3, brush, build_mat)
				elif smooth:
					world.smooth_sphere(p, brush * 1.5, 0.6, 1)


func _process(_dt: float) -> void:
	var s := world.get_stats()
	hud.text = "%d fps   %.2fM tris   %d chunk meshes   %d colliders   %d chunks   %d queued   %.2f ms/chunk on %d threads\nbrush %.1f m   build: %s   gold: %.1f m³%s\nLMB dig  RMB build  Q smooth  E block  F bomb  wheel size  1-5 material  V fly  R new world  P profile: %s" % [
		Engine.get_frames_per_second(), s.triangles / 1e6, s.meshes, s.bodies, s.chunks, s.pending, s.mesh_ms, s.threads,
		brush, _material(build_mat).name, gold, "   [FLY]" if fly else "", profile]
	if shot_path != "" and spawned and s.pending == 0 and Engine.get_process_frames() > 60:
		_beauty_shot()


## Turns an edit result into feedback: gold tally and tumbling fragments of whatever was removed.
func _dug(result: Dictionary, at: Vector3, force: float) -> void:
	var removed: Dictionary = result.get("removed", {})
	gold += removed.get(5, 0.0)
	for id in removed:
		for i in clampi(int(removed[id] * 2.5) + 1, 1, 10):
			_spawn_fragment(at + Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5) * brush, id, force)


func _spawn_fragment(at: Vector3, id: int, force: float) -> void:
	if debris.size() >= 220:
		debris.pop_front().queue_free()
	if not debris_mats.has(id):
		var d := _material(id)
		var m := StandardMaterial3D.new()
		m.albedo_color = d.color_a.lerp(d.color_b, 0.5)
		m.roughness = d.roughness
		m.metallic = d.metallic
		debris_mats[id] = m
	var b := RigidBody3D.new()
	var mi := MeshInstance3D.new()
	mi.mesh = box_mesh
	mi.material_override = debris_mats[id]
	var cs := CollisionShape3D.new()
	cs.shape = box_shape
	b.add_child(mi)
	b.add_child(cs)
	b.scale = Vector3.ONE # RigidBody ignores scale; size the children instead
	var size := Vector3(randf_range(0.12, 0.4), randf_range(0.12, 0.35), randf_range(0.12, 0.4))
	mi.scale = size
	cs.scale = size
	b.mass = size.x * size.y * size.z * 40.0
	b.collision_layer = 2
	b.collision_mask = 1 | 2
	b.position = at
	b.linear_velocity = (Vector3(randf() - 0.5, randf() * 0.9 + 0.3, randf() - 0.5)).normalized() * randf_range(0.5, 1.0) * force
	b.angular_velocity = Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5) * 12.0
	add_child(b)
	debris.append(b)
	get_tree().create_timer(randf_range(4.0, 7.0)).timeout.connect(func() -> void:
		if is_instance_valid(b):
			debris.erase(b)
			b.queue_free())


func _place_block() -> void:
	var hit := world.raycast(cam.global_position, -cam.global_basis.z, 14.0)
	if hit.is_empty():
		return
	var p: Vector3 = (hit.position + hit.normal * 0.9).snappedf(0.5)
	if p.distance_to(player.global_position) > 2.2:
		world.build_box(Transform3D(Basis(Vector3.UP, snappedf(player.rotation.y, PI / 8.0)), p), Vector3.ONE, build_mat)


func _throw_bomb() -> void:
	var b := RigidBody3D.new()
	var mi := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.3
	sm.height = 0.6
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.08, 0.08, 0.1)
	m.emission_enabled = true
	m.emission = Color(1.0, 0.3, 0.05)
	sm.material = m
	mi.mesh = sm
	var cs := CollisionShape3D.new()
	var sp := SphereShape3D.new()
	sp.radius = 0.3
	cs.shape = sp
	b.add_child(mi)
	b.add_child(cs)
	b.collision_layer = 2
	b.collision_mask = 1
	b.continuous_cd = true
	b.position = cam.global_position - cam.global_basis.z * 1.0
	b.linear_velocity = -cam.global_basis.z * 22.0 + Vector3.UP * 3.0
	add_child(b)
	await get_tree().create_timer(1.8).timeout
	if not is_instance_valid(b):
		return
	var at := b.global_position
	b.queue_free()
	var old := brush
	brush = 4.5
	_dug(world.dig_sphere(at, 4.5, 2), at, 13.0)
	brush = old
	for d in debris: # shove loose fragments away from the blast
		if is_instance_valid(d) and d.global_position.distance_to(at) < 9.0:
			d.apply_central_impulse((d.global_position - at).normalized() * d.mass * 14.0)
	var flash := OmniLight3D.new()
	flash.light_color = Color(1.0, 0.6, 0.2)
	flash.light_energy = 12.0
	flash.omni_range = 22.0
	flash.position = at
	add_child(flash)
	var tw := create_tween()
	tw.tween_property(flash, "light_energy", 0.0, 0.35)
	tw.tween_callback(flash.queue_free)


## --shot: stage three views (ruins, panorama, a blast crater), save <path>_N.png, quit.
func _beauty_shot() -> void:
	set_process(false)
	fly = true
	if profile == "level":
		var c := level.get_level_size() * 0.5
		await _shoot([[c + Vector3(-45, 35, 55), c], [level.get_spawn_point() + Vector3(0, 4, 6), c + Vector3(0, 2, 0)]])
		return
	var tower := _ground(Vector3(14.0, 0, -6.0))
	world.dig_sphere(tower + Vector3(-9, 0.5, 6), 3.2, 2)
	world.dig_sphere(tower + Vector3(-12, -1.5, 3), 2.6, 2)
	world.build_sphere(tower + Vector3(-5, 1.0, 12), 1.8, 8)
	var crater := _ground(Vector3(-30.0, 0, 20.0))
	for i in 4:
		world.dig_sphere(crater + Vector3(i * 2.5, -1.0 - i * 2.2, 0), 5.0, 2)
	await _shoot([
		[tower + Vector3(-20, 14, 30), tower + Vector3(-6, 3, 2)],
		[Vector3(-70.0, 60.0, 85.0), Vector3(0, 20, 0)],
		[crater + Vector3(-9, 5, 7), crater + Vector3(4, -5, 0)],
	])


func _shoot(views: Array) -> void:
	for i in views.size():
		player.global_position = views[i][0] - cam.position
		cam.look_at(views[i][1])
		await get_tree().create_timer(0.3).timeout
		var quiet := 0
		while quiet < 3: # let the stream around the new viewpoint settle
			await get_tree().process_frame
			quiet = quiet + 1 if world.get_pending_count() == 0 else 0
		var t0 := Time.get_ticks_usec()
		for f in 100:
			await get_tree().process_frame
		print("view %d: %.0f fps" % [i, 100.0e6 / (Time.get_ticks_usec() - t0)])
		get_viewport().get_texture().get_image().save_png("%s_%d.png" % [shot_path.trim_suffix(".png"), i])
	print("shots saved ", world.get_stats())
	get_tree().quit()
