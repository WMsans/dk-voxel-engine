class_name VoxelBomb
extends RigidBody3D
## A thrown charge: after `fuse` seconds it digs a sphere out of `world`, flashes and frees itself.

signal exploded(result: Dictionary, at: Vector3, radius: float)

@export var fuse := 1.8
@export var radius := 4.5
@export var power := 2 ## dig power: materials harder than this survive

var world: VoxelWorld


func _ready() -> void:
	get_tree().create_timer(fuse).timeout.connect(_explode)


func _explode() -> void:
	var at := global_position
	exploded.emit(world.dig_sphere(at, radius, power), at, radius)
	var flash := OmniLight3D.new()
	flash.light_color = Color(1.0, 0.6, 0.2)
	flash.light_energy = 12.0
	flash.omni_range = radius * 5.0
	get_parent().add_child(flash)
	flash.global_position = at
	var tw := flash.create_tween()
	tw.tween_property(flash, "light_energy", 0.0, 0.35)
	tw.tween_callback(flash.queue_free)
	queue_free()
