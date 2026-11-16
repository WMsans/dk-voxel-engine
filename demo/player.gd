extends CharacterBody3D
## The player node: walking, flying, mouse look and the dig/build/smooth tools. Edits go through `world`,
## which Main assigns before the first frame.

const BUILD_KEYS := [7, 2, 3, 4, 8]

var world: VoxelWorld

@onready var cam: Camera3D = $Camera3D

var brush := 1.6
var build_mat := 7
var fly := false
var spawned := false
var cooldown := 0.0


func _unhandled_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-e.relative.x * 0.0025)
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
			KEY_F: world.throw_bomb(cam.global_position, cam.global_basis.z)
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
		global_position += v * dt
		velocity = Vector3.ZERO
	else:
		var wish := global_basis * Vector3(dir.x, 0, dir.y).limit_length() * speed
		velocity.x = move_toward(velocity.x, wish.x, 60.0 * dt)
		velocity.z = move_toward(velocity.z, wish.z, 60.0 * dt)
		velocity.y -= 24.0 * dt
		if is_on_floor() and Input.is_physical_key_pressed(KEY_SPACE):
			velocity.y = 9.0
		move_and_slide()
		if position.y < -20.0: # fell out of the world
			position = world.respawn_point(position) + Vector3.UP * 2.0
			velocity = Vector3.ZERO

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
					world.dig_feedback(world.dig_sphere(p - hit.normal * brush * 0.3, brush, 1), p, 4.0, brush)
				elif rmb and p.distance_to(cam.global_position) > brush + 1.2:
					world.build_sphere(p + hit.normal * brush * 0.3, brush, build_mat)
				elif smooth:
					world.smooth_sphere(p, brush * 1.5, 0.6, 1)


func _place_block() -> void:
	var hit := world.raycast(cam.global_position, -cam.global_basis.z, 14.0)
	if hit.is_empty():
		return
	var p: Vector3 = (hit.position + hit.normal * 0.9).snappedf(0.5)
	if p.distance_to(global_position) > 2.2:
		world.build_box(Transform3D(Basis(Vector3.UP, snappedf(rotation.y, PI / 8.0)), p), Vector3.ONE, build_mat)
