class_name BeautyShot
## Renders a list of [camera position, look-at target] views to <path>_N.png, printing fps per view.

static func shoot(world: VoxelWorld, cam: Camera3D, views: Array, path: String) -> void:
	var tree := world.get_tree()
	var mover := cam.get_parent() as Node3D # move the body, so its VoxelViewer streams the view in
	for i in views.size():
		mover.global_position = views[i][0] - cam.position
		cam.look_at(views[i][1])
		await tree.create_timer(0.3).timeout
		var quiet := 0
		while quiet < 3: # let the stream around the new viewpoint settle
			await tree.process_frame
			quiet = quiet + 1 if world.get_pending_count() == 0 else 0
		var t0 := Time.get_ticks_usec()
		for f in 100:
			await tree.process_frame
		print("view %d: %.0f fps" % [i, 100.0e6 / (Time.get_ticks_usec() - t0)])
		world.get_viewport().get_texture().get_image().save_png("%s_%d.png" % [path.trim_suffix(".png"), i])
	print("shots saved ", world.get_stats())
