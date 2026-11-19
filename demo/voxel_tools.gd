class_name VoxelTools
extends Node
## Dig/build/smooth/block/bomb tools aimed by `camera`. Put it under the body that carries the camera.
##   LMB dig   RMB build   Q smooth   E sharp block   F bomb   wheel brush size   1-5 build material
## Every removal (dig or bomb) is reported through `dug`; connect VoxelDebris.burst or game rules to it.

signal dug(result: Dictionary, at: Vector3, force: float, radius: float)

@export var world: VoxelWorld
@export var camera: Camera3D
@export var bomb_scene: PackedScene ## root must be a VoxelBomb
@export var build_materials: Array[int] = [7, 2, 3, 4, 8] ## keys 1-5
@export var dig_power := 1
@export var reach := 14.0

var active := true ## main turns tools off until the player has ground under them
var brush := 1.6
var build_mat := 7
var cooldown := 0.0

@onready var body := get_parent() as Node3D


func _unhandled_input(e: InputEvent) -> void:
	if not active or Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	if e is InputEventMouseButton and e.pressed:
		if e.button_index == MOUSE_BUTTON_WHEEL_UP:
			brush = minf(brush + 0.2, 5.0)
		elif e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			brush = maxf(brush - 0.2, 0.6)
	elif e is InputEventKey and e.pressed and not e.echo:
		match e.keycode:
			KEY_F: throw_bomb()
			KEY_E: place_block()
			KEY_1, KEY_2, KEY_3, KEY_4, KEY_5:
				var i: int = e.keycode - KEY_1
				if i < build_materials.size():
					build_mat = build_materials[i]


func _physics_process(dt: float) -> void:
	cooldown -= dt
	if not active or cooldown > 0.0 or Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	var lmb := Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	var rmb := Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
	var smooth := Input.is_physical_key_pressed(KEY_Q)
	if not (lmb or rmb or smooth):
		return
	var hit := _aim()
	if hit.is_empty():
		return
	cooldown = 0.07
	var p: Vector3 = hit.position
	if lmb:
		dug.emit(world.dig_sphere(p - hit.normal * brush * 0.3, brush, dig_power), p, 4.0, brush)
	elif rmb and p.distance_to(camera.global_position) > brush + 1.2:
		world.build_sphere(p + hit.normal * brush * 0.3, brush, build_mat)
	elif smooth:
		world.smooth_sphere(p, brush * 1.5, 0.6, 1)


## A 1 m sharp-edged cube on the aimed face, snapped to the half-metre grid and to 22.5° of the body's yaw.
func place_block() -> void:
	var hit := _aim()
	if hit.is_empty():
		return
	var p: Vector3 = (hit.position + hit.normal * 0.9).snappedf(0.5)
	if p.distance_to(body.global_position) > 2.2:
		world.build_box(Transform3D(Basis(Vector3.UP, snappedf(body.global_rotation.y, PI / 8.0)), p), Vector3.ONE, build_mat)


func throw_bomb() -> void:
	var forward := camera.global_basis.z
	var b: VoxelBomb = bomb_scene.instantiate()
	b.world = world
	b.position = camera.global_position - forward
	b.linear_velocity = -forward * 22.0 + Vector3.UP * 3.0
	b.exploded.connect(func(result: Dictionary, at: Vector3, radius: float) -> void: dug.emit(result, at, 13.0, radius))
	world.add_child(b)


func _aim() -> Dictionary:
	return world.raycast(camera.global_position, -camera.global_basis.z, reach)
