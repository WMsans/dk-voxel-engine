class_name FlyWalkController
extends CharacterBody3D
## First-person body: walking, flying and mouse look. Knows nothing about voxels; emits `fell_out` below
## `fall_limit` and lets the owner decide where to put it back.
##   WASD move   Space jump (up when flying)   Ctrl down   Shift sprint   V toggle fly   Esc free the mouse

signal fell_out

@export var fall_limit := -20.0

var active := true ## held still while false (main waits for ground collision before spawning)
var fly := false

@onready var cam: Camera3D = $Camera3D


func _unhandled_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-e.relative.x * 0.0025)
		cam.rotation.x = clampf(cam.rotation.x - e.relative.y * 0.0025, -1.5, 1.5)
	elif e is InputEventMouseButton and e.pressed and Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		get_viewport().set_input_as_handled() # the click that grabs the mouse must not dig
	elif e is InputEventKey and e.pressed and not e.echo:
		match e.keycode:
			KEY_ESCAPE: Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
			KEY_V: fly = not fly


func _physics_process(dt: float) -> void:
	if not active:
		return
	var dir := Input.get_vector(&"ui_left", &"ui_right", &"ui_up", &"ui_down")
	dir += Vector2(float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)), float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
	var speed := 14.0 if Input.is_physical_key_pressed(KEY_SHIFT) else 7.0
	if fly:
		var v := cam.global_basis * Vector3(dir.x, 0, dir.y) * speed * 2.0
		v.y += (float(Input.is_physical_key_pressed(KEY_SPACE)) - float(Input.is_physical_key_pressed(KEY_CTRL))) * speed * 2.0
		global_position += v * dt
		velocity = Vector3.ZERO
		return
	var wish := global_basis * Vector3(dir.x, 0, dir.y).limit_length() * speed
	velocity.x = move_toward(velocity.x, wish.x, 60.0 * dt)
	velocity.z = move_toward(velocity.z, wish.z, 60.0 * dt)
	velocity.y -= 24.0 * dt
	if is_on_floor() and Input.is_physical_key_pressed(KEY_SPACE):
		velocity.y = 9.0
	move_and_slide()
	if position.y < fall_limit:
		velocity = Vector3.ZERO
		fell_out.emit()
