extends VoxelWorld
## The demo world node: material set, terrain profile, ruins, and the debris and bombs of an edit.

const MATERIALS := [ # name, albedo, normal/roughness/AO, tiles per metre, roughness factor, metallic, emission, hardness
	["Default", "soil", "soil", 0.5, 0.9, 0.0, 0.0, 0],
	["Grass", "grass", "grass", 0.22, 0.95, 0.0, 0.0, 0],
	["Soil", "soil", "soil", 0.3, 0.95, 0.0, 0.0, 0],
	["Rock", "rock", "rock", 0.16, 0.8, 0.0, 0.0, 1],
	["Sand", "sand", "sand", 0.4, 1.0, 0.0, 0.0, 0],
	["Gold", "gold", "gold", 0.35, 0.3, 1.0, 0.2, 1],
	["Bedrock", "bedrock", "bedrock", 0.14, 0.6, 0.0, 0.0, 255],
	["Brick", "brick", "brick", 0.3, 0.85, 0.0, 0.0, 1],
	["Crystal", "crystal", "crystal", 0.2, 0.15, 0.3, 1.2, 2],
]
const CELL := 0.5
const GROUND_FROM := 400.0 # raycast down from here to find the ground: endless worlds have no height
const LEVEL_PATH := "res://demo/levels/mini_level.dkvl"

var procedural := DemoTerrainGenerator.new()
var level := LevelTerrainGenerator.new()
var profile := "level"
var gold := 0.0
var debris: Array[RigidBody3D] = []
var debris_mats := {}
var box_mesh := BoxMesh.new()
var box_shape := BoxShape3D.new()


func _ready() -> void:
	level.level_path = LEVEL_PATH
	cell_size = CELL
	material_set = _make_material_set()
	material = _make_terrain_material()


func new_world(p_profile: String, world_seed: int) -> void:
	profile = p_profile
	procedural.seed = world_seed
	if profile == "level":
		generator = level
		bounds_size = Vector3i((level.get_level_size() / (16.0 * cell_size)).ceil()) # bounded, like a Bananza VoxelField
	else:
		generator = procedural
		bounds_size = Vector3i.ZERO # endless: chunks stream around the player's viewer
	reset()


func spawn_position() -> Vector3:
	return level.get_spawn_point() if profile == "level" else Vector3.ZERO


## Where a player who fell out of the world comes back: at the level spawn, or above the ground below them.
func respawn_point(at: Vector3) -> Vector3:
	return ground(spawn_position() if profile == "level" else at)


func ground(at: Vector3) -> Vector3:
	var hit := raycast(Vector3(at.x, GROUND_FROM, at.z), Vector3.DOWN, GROUND_FROM + 200.0)
	return hit.get("position", Vector3(at.x, 20.0, at.z))


## Waits (bounded, so a stalled stream cannot hang the demo) until the chunks under `aabb` are meshed.
func wait_ready(aabb: AABB, need_collision: bool, frames := 600) -> void:
	for i in frames:
		if is_area_ready(aabb, need_collision):
			return
		await get_tree().process_frame


func material_name(id: int) -> String:
	return _material(id).name


func _material(id: int) -> VoxelMaterialDef:
	var defs: Array = material_set.materials
	if id < 0 or id >= defs.size():
		id = 0
	return defs[id]


func _make_material_set() -> VoxelMaterialSet:
	var set := VoxelMaterialSet.new()
	var defs: Array[VoxelMaterialDef] = []
	for m in MATERIALS:
		var d := VoxelMaterialDef.new()
		d.name = m[0]
		# Every surface is a real image file; tints are baked into the file by demo/textures/make_textures.py.
		d.albedo = load("res://demo/textures/%s_albedo.png" % m[1])
		d.normal = load("res://demo/textures/%s_normal.png" % m[2])
		d.roughness_map = load("res://demo/textures/%s_roughness.png" % m[2])
		d.ao_map = load("res://demo/textures/%s_ao.png" % m[2])
		d.color_a = Color.WHITE # leave the albedo as authored: the tint ramp stays out of the way
		d.color_b = Color.WHITE
		d.pattern_scale = m[3]
		d.roughness = m[4]
		d.metallic = m[5]
		d.emission = m[6]
		d.hardness = m[7]
		defs.append(d)
	set.materials = defs
	return set


func _make_terrain_material() -> ShaderMaterial:
	# Colours, textures and parameters all come from the material set; the shader needs no inputs of its own.
	var mat := ShaderMaterial.new()
	mat.shader = load("res://addons/dk_voxel/voxel_terrain.gdshader")
	return mat


## Sharp-edged structures: the Hermite path of the mesher. Everything here is plain CSG on the voxel field.
func build_ruins(at: Vector3) -> void:
	var g := ground(at)
	var base := Transform3D(Basis(Vector3.UP, 0.4), g + Vector3.UP * 3.0)
	build_box(base, Vector3(5, 4.5, 5), 7)
	dig_box(base.translated_local(Vector3(0, 0.5, 0)), Vector3(4, 4.0, 4), 9)
	dig_box(base.translated_local(Vector3(0, -1.0, 5)), Vector3(1.2, 2.2, 1.5), 9) # door
	dig_cylinder(base.translated_local(Vector3(5, 1.0, 0)) * Transform3D(Basis(Vector3.FORWARD, PI * 0.5), Vector3.ZERO), 1.4, 1.5, 9) # round window
	for i in 5: # colonnade
		var p := ground(at + Vector3(-10.0 - i * 4.0, 0, 9.0))
		build_cylinder(Transform3D(Basis(), p + Vector3.UP * 3.0), 0.8, 4.0, 3)
		build_box(Transform3D(Basis(), p + Vector3.UP * 7.3), Vector3(1.3, 0.35, 1.3), 3)
	for i in 8: # stairs up to the tower
		build_box(base.translated_local(Vector3(0, -4.2 + i * 0.5, 13.0 - i * 1.0)), Vector3(1.6, 0.25, 0.5), 3)


## Turns an edit result into feedback: gold tally and tumbling fragments of whatever was removed.
func dig_feedback(result: Dictionary, at: Vector3, force: float, radius: float) -> void:
	var removed: Dictionary = result.get("removed", {})
	gold += removed.get(5, 0.0)
	for id in removed:
		for i in clampi(int(removed[id] * 2.5) + 1, 1, 10):
			_spawn_fragment(at + Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5) * radius, id, force)


func _spawn_fragment(at: Vector3, id: int, force: float) -> void:
	if debris.size() >= 220:
		debris.pop_front().queue_free()
	if not debris_mats.has(id):
		var d := _material(id)
		var m := StandardMaterial3D.new()
		m.albedo_texture = d.albedo
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


func throw_bomb(from: Vector3, forward: Vector3) -> void:
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
	b.position = from - forward * 1.0
	b.linear_velocity = -forward * 22.0 + Vector3.UP * 3.0
	add_child(b)
	await get_tree().create_timer(1.8).timeout
	if not is_instance_valid(b):
		return
	var at := b.global_position
	b.queue_free()
	dig_feedback(dig_sphere(at, 4.5, 2), at, 13.0, 4.5)
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
