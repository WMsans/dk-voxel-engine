extends Node3D
## Demo for the dk_voxel engine: a destructible dual-contoured world.
##   R new world   P switch profile   (movement: player.gd, tools: voxel_tools.gd)
## Command line (after --):  --shot=<png>  render beauty shots and quit
##                           --profile=procedural|level  which terrain generation profile to start with
## Profiles: "procedural" = DemoTerrainGenerator, endless; "level" = LevelTerrainGenerator, the hand-made
## Blender level in demo/levels (Bananza-style: bounded, every part authored).
##
## This script is the demo's game: it owns the profiles, spawning, the gold rule and the demo content
## (ruins, trees). Every other node is a standalone piece wired here or in main.tscn:
##   World (plain VoxelWorld)  Player (FlyWalkController)  Player/Tools (VoxelTools)
##   Debris (VoxelDebris)  HUD  -- and VoxelUtil / BeautyShot static helpers.

const LEVEL_PATH := "res://demo/levels/mini_level.dkvl"
const GOLD := 5
const BARK := 17
const LEAVES := 18
## The trees of the anime-tree-rendering project (made in its blender/*.blend), voxelized when planted.
const TREES: Array[PackedScene] = [
	preload("res://demo/trees/tree_a.glb"), preload("res://demo/trees/tree_b.glb"), preload("res://demo/trees/tree_c.glb"),
	preload("res://demo/trees/pine_a.glb"), preload("res://demo/trees/pine_b.glb"),
]
const TREE_SCALE := 1.6 # at 0.5 m cells the thinnest branches need it to survive voxelizing
const LEVEL_TREE := Vector3(-7.0, 0, -6.0) # from the level's spawn

@onready var world: VoxelWorld = $World
@onready var player: FlyWalkController = $Player
@onready var tools: VoxelTools = $Player/Tools
@onready var debris: VoxelDebris = $Debris
@onready var hud: CanvasLayer = $HUD

var procedural := DemoTerrainGenerator.new()
var level := LevelTerrainGenerator.new()
var profile := "level"
var gold := 0.0
var spawn_token := 0
var shot_path := ""


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--shot="):
			shot_path = a.trim_prefix("--shot=")
		elif a.begins_with("--profile="):
			profile = a.trim_prefix("--profile=")
	level.level_path = LEVEL_PATH
	(world.material as ShaderMaterial).set_shader_parameter(&"leaf_material", LEAVES) # cel shader's scallop marker
	tools.dug.connect(debris.burst)
	tools.dug.connect(func(result: Dictionary, _at: Vector3, _f: float, _r: float) -> void:
		gold += result.get("removed", {}).get(GOLD, 0.0))
	player.fell_out.connect(func() -> void:
		player.position = VoxelUtil.ground(world, spawn_position() if profile == "level" else player.position) + Vector3.UP * 2.0)
	hud.visible = shot_path == ""
	_new_world(7)
	if shot_path == "":
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


func _new_world(world_seed: int) -> void:
	procedural.seed = world_seed
	if profile == "level":
		world.generator = level
		world.bounds_size = Vector3i((level.get_level_size() / (16.0 * world.cell_size)).ceil()) # bounded, like a Bananza VoxelField
	else:
		world.generator = procedural
		world.bounds_size = Vector3i.ZERO # endless: chunks stream around the player's viewer
	world.reset()
	spawn_token += 1 # a quick second R must not leave the old spawn building ruins
	_set_active(false)
	_spawn(spawn_token)


func spawn_position() -> Vector3:
	return level.get_spawn_point() if profile == "level" else Vector3.ZERO


func _set_active(on: bool) -> void:
	player.active = on
	tools.active = on


## Puts the player on the ground at the spawn, then builds the demo content, once the area is streamed in.
func _spawn(token: int) -> void:
	var at := spawn_position()
	# Move the player (and its viewer) to the spawn point before waiting: the origin is outside every
	# viewer's distance after flying away, so waiting there would time out.
	player.position = at + Vector3.UP * 20.0
	player.velocity = Vector3.ZERO
	await VoxelUtil.wait_ready(world, AABB(at + Vector3(-32, 0, -32), Vector3(64, 64, 64)), false) # ground and ruins
	if token != spawn_token:
		return
	player.position = VoxelUtil.ground(world, at) + Vector3.UP * 3.0
	player.velocity = Vector3.ZERO
	if profile == "procedural":
		_build_ruins(at + Vector3(14.0, 0, -6.0))
		for i in 5: # a grove, one of each tree
			var a := TAU * i / 5
			_plant_tree(TREES[i], at + Vector3(-14.0, 0, 10.0) + Vector3(cos(a), 0, sin(a)) * 11.0, a * 2.3)
	elif profile == "level":
		_plant_tree(TREES[0], at + LEVEL_TREE)
	await VoxelUtil.wait_ready(world, AABB(player.position + Vector3(-8, -2, -8), Vector3(16, 12, 16)), true) # under the player
	if token != spawn_token:
		return
	_set_active(true)


func _plant_tree(scene: PackedScene, at: Vector3, angle := 0.0) -> void:
	var xf := Transform3D(Basis(Vector3.UP, angle).scaled(Vector3.ONE * TREE_SCALE), VoxelUtil.ground(world, at) + Vector3.DOWN * 0.5)
	VoxelUtil.plant_tree(world, scene, xf, BARK, LEAVES)


## Sharp-edged structures: the Hermite path of the mesher. Everything here is plain CSG on the voxel field.
func _build_ruins(at: Vector3) -> void:
	world.expose_inner_material = false # authored cuts: clean brick, not rubble
	var base := Transform3D(Basis(Vector3.UP, 0.4), VoxelUtil.ground(world, at) + Vector3.UP * 3.0)
	world.build_box(base, Vector3(5, 4.5, 5), 7)
	world.dig_box(base.translated_local(Vector3(0, 0.5, 0)), Vector3(4, 4.0, 4), 9)
	world.dig_box(base.translated_local(Vector3(0, -1.0, 5)), Vector3(1.2, 2.2, 1.5), 9) # door
	world.dig_cylinder(base.translated_local(Vector3(5, 1.0, 0)) * Transform3D(Basis(Vector3.FORWARD, PI * 0.5), Vector3.ZERO), 1.4, 1.5, 9) # round window
	for i in 5: # colonnade
		var p := VoxelUtil.ground(world, at + Vector3(-10.0 - i * 4.0, 0, 9.0))
		world.build_cylinder(Transform3D(Basis(), p + Vector3.UP * 3.0), 0.8, 4.0, 3)
		world.build_box(Transform3D(Basis(), p + Vector3.UP * 7.3), Vector3(1.3, 0.35, 1.3), 3)
	for i in 8: # stairs up to the tower
		world.build_box(base.translated_local(Vector3(0, -4.2 + i * 0.5, 13.0 - i * 1.0)), Vector3(1.6, 0.25, 0.5), 3)
	world.expose_inner_material = true


func _unhandled_input(e: InputEvent) -> void:
	if e is InputEventKey and e.pressed and not e.echo:
		match e.keycode:
			KEY_R: _new_world(randi())
			KEY_P:
				profile = "level" if profile == "procedural" else "procedural"
				_new_world(procedural.seed)


func _process(_dt: float) -> void:
	if hud.visible:
		var mat: VoxelMaterialDef = world.material_set.materials[tools.build_mat]
		hud.info = "brush %.1f m   build: %s   gold: %.1f m³%s\nLMB dig  RMB build  Q smooth  E block  F bomb  wheel size  1-5 material  V fly  R new world  P profile: %s" % [
			tools.brush, mat.name, gold, "   [FLY]" if player.fly else "", profile]
	if shot_path != "" and player.active and world.get_stats().pending == 0 and Engine.get_process_frames() > 60:
		_beauty_shot()


## --shot: stage the views of the current profile (for "procedural", cut a blast crater first), save, quit.
func _beauty_shot() -> void:
	set_process(false)
	player.fly = true
	var views: Array
	if profile == "level":
		var c: Vector3 = level.get_level_size() * 0.5
		var spawn := level.get_spawn_point()
		views = [[c + Vector3(-45, 35, 55), c], [spawn + Vector3(0, 4, 6), c + Vector3(0, 2, 0)],
				[spawn + LEVEL_TREE + Vector3(6, 6, 22), spawn + LEVEL_TREE + Vector3(0, 7, 0)]]
	else:
		var tower := VoxelUtil.ground(world, Vector3(14.0, 0, -6.0))
		world.dig_sphere(tower + Vector3(-9, 0.5, 6), 3.2, 2)
		world.dig_sphere(tower + Vector3(-12, -1.5, 3), 2.6, 2)
		world.build_sphere(tower + Vector3(-5, 1.0, 12), 1.8, 8)
		var crater := VoxelUtil.ground(world, Vector3(-30.0, 0, 20.0))
		for i in 4:
			world.dig_sphere(crater + Vector3(i * 2.5, -1.0 - i * 2.2, 0), 5.0, 2)
		var grove := VoxelUtil.ground(world, Vector3(-14.0, 0, 10.0))
		views = [
			[tower + Vector3(-20, 14, 30), tower + Vector3(-6, 3, 2)],
			[grove + Vector3(26, 12, 22), grove + Vector3(0, 8, 0)],
			[Vector3(-70.0, 60.0, 85.0), Vector3(0, 20, 0)],
			[crater + Vector3(-9, 5, 7), crater + Vector3(4, -5, 0)],
		]
	await BeautyShot.shoot(world, player.cam, views, shot_path)
	get_tree().quit()
