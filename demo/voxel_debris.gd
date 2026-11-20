class_name VoxelDebris
extends Node3D
## Tumbling fragments of whatever an edit removed. Connect an edit's result to burst(); fragments are
## textured from `material_set` (the inner material, if any) and parented to this node.
## Shards are baked by shards/make_shards.gd.

@export var shards: Array[Mesh] = []
@export var material_set: VoxelMaterialSet
@export var max_fragments := 220
@export_flags_3d_physics var collision_layer := 2
@export_flags_3d_physics var collision_mask := 1 | 2

var _fragments: Array[RigidBody3D] = []
var _mats := {}
var _shapes: Array[Shape3D] = []


func _ready() -> void:
	for m in shards:
		_shapes.append(m.create_convex_shape())


## `result` is what dig_sphere/dig_box return; spawns fragments of each removed material around `at`
## and shoves loose ones within 2 * radius away from it.
func burst(result: Dictionary, at: Vector3, force: float, radius: float) -> void:
	var removed: Dictionary = result.get("removed", {})
	for id in removed:
		for i in clampi(int(removed[id] * 2.5) + 1, 1, 10):
			_spawn(at + Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5) * radius, id, force)
	for d in _fragments:
		if is_instance_valid(d) and d.global_position.distance_to(at) < radius * 2.0:
			d.apply_central_impulse((d.global_position - at).normalized() * d.mass * force)


func _material(id: int) -> VoxelMaterialDef:
	var defs: Array = material_set.materials
	return defs[id if id >= 0 and id < defs.size() else 0]


func _spawn(at: Vector3, id: int, force: float) -> void:
	if shards.is_empty():
		return
	if _fragments.size() >= max_fragments:
		var old: RigidBody3D = _fragments.pop_front()
		if is_instance_valid(old):
			old.queue_free()
	if not _mats.has(id):
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
		_mats[id] = m
	var r := randi() % shards.size()
	var b := RigidBody3D.new()
	var mi := MeshInstance3D.new()
	mi.mesh = shards[r]
	mi.material_override = _mats[id]
	var cs := CollisionShape3D.new()
	cs.shape = _shapes[r]
	b.add_child(mi)
	b.add_child(cs)
	var size := randf_range(0.12, 0.4) # shards are unit-ish; uniform scale keeps the convex shape valid
	mi.scale = Vector3.ONE * size
	cs.scale = Vector3.ONE * size
	b.mass = size * size * size * 25.0
	b.collision_layer = collision_layer
	b.collision_mask = collision_mask
	b.position = at
	b.linear_velocity = (Vector3(randf() - 0.5, randf() * 0.9 + 0.3, randf() - 0.5)).normalized() * randf_range(0.5, 1.0) * force
	b.angular_velocity = Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5) * 12.0
	add_child(b)
	_fragments.append(b)
	get_tree().create_timer(randf_range(4.0, 7.0)).timeout.connect(func() -> void:
		if is_instance_valid(b):
			_fragments.erase(b)
			b.queue_free())
