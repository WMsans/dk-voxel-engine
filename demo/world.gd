extends VoxelWorld
## The demo world node: terrain profile, ruins, and the debris and bombs of an edit.
## Assets live in the scene: materials.tres (material set; ids 9+ are the broken insides a dig exposes),
## shards/shard_N.res (debris meshes, baked by shards/make_shards.gd) and bomb.tscn.

const GROUND_FROM := 400.0 # raycast down from here to find the ground: endless worlds have no height
const LEVEL_PATH := "res://demo/levels/mini_level.dkvl"
const BARK := 17
const LEAVES := 18
## The trees of the anime-tree-rendering project (made in its blender/*.blend), voxelized when planted.
const TREES: Array[PackedScene] = [
	preload("res://demo/trees/tree_a.glb"), preload("res://demo/trees/tree_b.glb"), preload("res://demo/trees/tree_c.glb"),
	preload("res://demo/trees/pine_a.glb"), preload("res://demo/trees/pine_b.glb"),
]
const TREE_SCALE := 1.6 # at 0.5 m cells the thinnest branches need it to survive voxelizing

@export var shards: Array[Mesh] = []
@export var bomb_scene: PackedScene

var procedural := DemoTerrainGenerator.new()
var level := LevelTerrainGenerator.new()
var profile := "level"
var gold := 0.0
var debris: Array[RigidBody3D] = []
var debris_mats := {}
var shard_shapes: Array[Shape3D] = []


func _ready() -> void:
	level.level_path = LEVEL_PATH
	(material as ShaderMaterial).set_shader_parameter(&"leaf_material", LEAVES) # cel shader's scallop marker
	for m in shards:
		shard_shapes.append(m.create_convex_shape())


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


## Sharp-edged structures: the Hermite path of the mesher. Everything here is plain CSG on the voxel field.
func build_ruins(at: Vector3) -> void:
	expose_inner_material = false # authored cuts: clean brick, not rubble
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
	expose_inner_material = true


## A grove around `at`, one of each tree.
func plant_trees(at: Vector3, count := 5) -> void:
	for i in count:
		var a := TAU * i / count
		plant_tree(TREES[i % TREES.size()], at + Vector3(cos(a), 0, sin(a)) * 11.0, a * 2.3)


## One tree on the ground below `at`: every mesh of the glb becomes a build_mesh edit, wood first so the
## leaves only fill the air around the branches.
func plant_tree(scene: PackedScene, at: Vector3, angle := 0.0) -> void:
	var xf := Transform3D(Basis(Vector3.UP, angle).scaled(Vector3.ONE * TREE_SCALE), ground(at) + Vector3.DOWN * 0.5)
	var tree := scene.instantiate()
	var parts := tree.find_children("*", "MeshInstance3D", true, false)
	parts.sort_custom(func(m, n) -> bool: return not _is_leaf(m) and _is_leaf(n))
	for mi: MeshInstance3D in parts:
		var local := Transform3D()
		var n: Node = mi
		while n != tree:
			local = (n as Node3D).transform * local
			n = n.get_parent()
		build_mesh(mi.mesh, xf * local, LEAVES if _is_leaf(mi) else BARK)
	tree.free()


func _is_leaf(mi: MeshInstance3D) -> bool:
	return mi.mesh.surface_get_material(0).resource_name.begins_with("Leaf")


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
		if d.inner_material >= 0: # fragments show the broken inside too (Bananza's IsInnerFragment)
			d = _material(d.inner_material)
		var m := StandardMaterial3D.new()
		m.albedo_texture = d.albedo
		m.normal_enabled = true
		m.normal_texture = d.normal
		m.uv1_triplanar = true # object-space projection, like Bananza's voxel texcoord: texture tumbles with the chip
		m.uv1_scale = Vector3.ONE * d.pattern_scale
		m.roughness = d.roughness
		m.metallic = d.metallic
		debris_mats[id] = m
	var r := randi() % shards.size()
	var b := RigidBody3D.new()
	var mi := MeshInstance3D.new()
	mi.mesh = shards[r]
	mi.material_override = debris_mats[id]
	var cs := CollisionShape3D.new()
	cs.shape = shard_shapes[r]
	b.add_child(mi)
	b.add_child(cs)
	var size := randf_range(0.12, 0.4) # shards are unit-ish; uniform scale keeps the convex shape valid
	mi.scale = Vector3.ONE * size
	cs.scale = Vector3.ONE * size
	b.mass = size * size * size * 25.0
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
	var b: RigidBody3D = bomb_scene.instantiate()
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
