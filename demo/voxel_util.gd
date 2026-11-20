class_name VoxelUtil
## Stateless helpers over any VoxelWorld: ground lookup, waiting for streamed chunks, stamping a glb in.

const GROUND_FROM := 400.0 # raycast down from here to find the ground: endless worlds have no height


## The ground below `at` (x/z used), or y = 20 if nothing is meshed there yet.
static func ground(world: VoxelWorld, at: Vector3) -> Vector3:
	var hit := world.raycast(Vector3(at.x, GROUND_FROM, at.z), Vector3.DOWN, GROUND_FROM + 200.0)
	return hit.get("position", Vector3(at.x, 20.0, at.z))


## Waits (bounded, so a stalled stream cannot hang the game) until the chunks under `aabb` are meshed.
static func wait_ready(world: VoxelWorld, aabb: AABB, need_collision: bool, frames := 600) -> void:
	for i in frames:
		if world.is_area_ready(aabb, need_collision):
			return
		await world.get_tree().process_frame


## Draws each [mesh, material] pair once, tiny, in front of `cam` for two frames, so its shaders and pipelines
## compile during loading instead of stalling the frame something first uses them. An exported game bakes its
## shaders; this also covers a run from the editor and the pipelines.
static func prewarm(cam: Camera3D, pairs: Array) -> void:
	var nodes: Array[MeshInstance3D] = []
	for pair in pairs:
		var mi := MeshInstance3D.new()
		mi.mesh = pair[0]
		mi.material_override = pair[1]
		mi.scale = Vector3.ONE * 0.001
		mi.position = Vector3(0, 0, -1)
		cam.add_child(mi)
		nodes.append(mi)
	for i in 2:
		await cam.get_tree().process_frame
	for mi in nodes:
		mi.queue_free()


## Stamps every mesh of `scene` into the world at `xf`: one build_mesh edit per MeshInstance3D. Meshes whose
## first surface material is named "Leaf*" get `leaf_material` and go last, so leaves only fill the air around
## the wood; everything else gets `wood_material`.
static func plant_tree(world: VoxelWorld, scene: PackedScene, xf: Transform3D, wood_material: int, leaf_material: int) -> void:
	var tree := scene.instantiate()
	var parts := tree.find_children("*", "MeshInstance3D", true, false)
	parts.sort_custom(func(m, n) -> bool: return not _is_leaf(m) and _is_leaf(n))
	for mi: MeshInstance3D in parts:
		var local := Transform3D()
		var n: Node = mi
		while n != tree:
			local = (n as Node3D).transform * local
			n = n.get_parent()
		world.build_mesh(mi.mesh, xf * local, leaf_material if _is_leaf(mi) else wood_material)
	tree.free()


static func _is_leaf(mi: MeshInstance3D) -> bool:
	var m := mi.mesh.surface_get_material(0)
	return m != null and m.resource_name.begins_with("Leaf")
