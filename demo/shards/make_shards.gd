extends SceneTree
## Bakes the debris shard meshes: low-poly faceted chips like Bananza's Eft_Fragment primitives, a coarse
## sphere squashed, its vertices jittered and sliced flat by a couple of fracture planes, flat shaded.
##     godot --headless --path . --script res://demo/shards/make_shards.gd

const COUNT := 10


func _init() -> void:
	seed(7)
	for i in COUNT:
		var path := "res://demo/shards/shard_%d.res" % i
		var err := ResourceSaver.save(_make_shard(), path)
		print(path, " ", error_string(err))
	quit()


func _make_shard() -> ArrayMesh:
	var sphere := SphereMesh.new()
	sphere.radial_segments = randi_range(5, 7)
	sphere.rings = randi_range(3, 4)
	var arrays := sphere.get_mesh_arrays()
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var aspect := Vector3(randf_range(0.8, 1.3), randf_range(0.55, 1.0), randf_range(0.8, 1.3))
	var cuts: Array[Plane] = []
	for i in randi_range(1, 3):
		cuts.append(Plane(Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5).normalized(), randf_range(0.15, 0.35)))
	var moved := {} # seam/pole duplicates share a position: move them together so the chip stays closed
	for i in verts.size():
		var key := verts[i].snapped(Vector3.ONE * 0.001)
		if not moved.has(key):
			var v := (verts[i] + Vector3(randf() - 0.5, randf() - 0.5, randf() - 0.5) * 0.25) * aspect
			for c in cuts:
				v -= c.normal * maxf(c.distance_to(v), 0.0)
			moved[key] = v
		verts[i] = moved[key]
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	st.set_smooth_group(-1) # flat facets
	for i in arrays[Mesh.ARRAY_INDEX]:
		st.add_vertex(verts[i])
	st.generate_normals()
	return st.commit()
