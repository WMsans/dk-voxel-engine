extends Node3D
## Demo for the dk_voxel engine: a destructible dual-contoured world.
##   WASD move, Space jump, Shift sprint, V toggle fly
##   LMB dig   RMB build   Q smooth   E place a sharp block   F throw a bomb
##   Wheel brush size   1-5 build material   R new world   P switch profile   Esc free the mouse
## Command line (after --):  --shot=<png>  render a beauty shot and quit
##                           --profile=procedural|level  which terrain generation profile to start with
## Profiles: "procedural" = DemoTerrainGenerator, endless; "level" = LevelTerrainGenerator, the hand-made
## Blender level in demo/levels (Bananza-style: bounded, every part authored).
## The scene holds the pieces: World (terrain and edits), Player (movement, tools), HUD (overlay).
## This script picks the profile, spawns the player and takes the screenshots.

@onready var world: VoxelWorld = $World
@onready var player: CharacterBody3D = $Player
@onready var hud: CanvasLayer = $HUD

var profile := "terrain"
var spawn_token := 0
var shot_path := ""


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--shot="):
			shot_path = a.trim_prefix("--shot=")
		elif a.begins_with("--profile="):
			profile = a.trim_prefix("--profile=")
	player.world = world
	hud.world = world
	hud.player = player
	hud.visible = shot_path == ""
	_new_world(7)
	if shot_path == "":
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


func _new_world(world_seed: int) -> void:
	world.new_world(profile, world_seed)
	hud.profile = profile
	spawn_token += 1 # a quick second R must not leave the old spawn building ruins
	player.spawned = false
	_spawn(spawn_token)


## Puts the player on the ground at the origin, then builds the ruins, once the area is streamed in.
func _spawn(token: int) -> void:
	var at: Vector3 = world.spawn_position()
	# Move the player (and its viewer) to the spawn point before waiting: the origin is outside every
	# viewer's distance after flying away, so waiting there would time out.
	player.position = at + Vector3.UP * 20.0
	player.velocity = Vector3.ZERO
	await world.wait_ready(AABB(at + Vector3(-32, 0, -32), Vector3(64, 64, 64)), false) # ground and ruins
	if token != spawn_token:
		return
	player.position = world.ground(at) + Vector3.UP * 3.0
	player.velocity = Vector3.ZERO
	if profile == "procedural":
		world.build_ruins(at + Vector3(14.0, 0, -6.0))
	await world.wait_ready(AABB(player.position + Vector3(-8, -2, -8), Vector3(16, 12, 16)), true) # under the player
	if token != spawn_token:
		return
	player.spawned = true


func _unhandled_input(e: InputEvent) -> void:
	if e is InputEventKey and e.pressed and not e.echo:
		match e.keycode:
			KEY_R: _new_world(randi())
			KEY_P:
				profile = "level" if profile == "procedural" else "procedural"
				_new_world(world.procedural.seed)


func _process(_dt: float) -> void:
	if shot_path != "" and player.spawned and world.get_stats().pending == 0 and Engine.get_process_frames() > 60:
		_beauty_shot()


## --shot: stage three views (ruins, panorama, a blast crater), save <path>_N.png, quit.
func _beauty_shot() -> void:
	set_process(false)
	player.fly = true
	if profile == "level":
		var c: Vector3 = world.level.get_level_size() * 0.5
		await _shoot([[c + Vector3(-45, 35, 55), c], [world.level.get_spawn_point() + Vector3(0, 4, 6), c + Vector3(0, 2, 0)]])
		return
	var tower: Vector3 = world.ground(Vector3(14.0, 0, -6.0))
	world.dig_sphere(tower + Vector3(-9, 0.5, 6), 3.2, 2)
	world.dig_sphere(tower + Vector3(-12, -1.5, 3), 2.6, 2)
	world.build_sphere(tower + Vector3(-5, 1.0, 12), 1.8, 8)
	var crater: Vector3 = world.ground(Vector3(-30.0, 0, 20.0))
	for i in 4:
		world.dig_sphere(crater + Vector3(i * 2.5, -1.0 - i * 2.2, 0), 5.0, 2)
	await _shoot([
		[tower + Vector3(-20, 14, 30), tower + Vector3(-6, 3, 2)],
		[Vector3(-70.0, 60.0, 85.0), Vector3(0, 20, 0)],
		[crater + Vector3(-9, 5, 7), crater + Vector3(4, -5, 0)],
	])


func _shoot(views: Array) -> void:
	for i in views.size():
		player.global_position = views[i][0] - player.cam.position
		player.cam.look_at(views[i][1])
		await get_tree().create_timer(0.3).timeout
		var quiet := 0
		while quiet < 3: # let the stream around the new viewpoint settle
			await get_tree().process_frame
			quiet = quiet + 1 if world.get_pending_count() == 0 else 0
		var t0 := Time.get_ticks_usec()
		for f in 100:
			await get_tree().process_frame
		print("view %d: %.0f fps" % [i, 100.0e6 / (Time.get_ticks_usec() - t0)])
		get_viewport().get_texture().get_image().save_png("%s_%d.png" % [shot_path.trim_suffix(".png"), i])
	print("shots saved ", world.get_stats())
	get_tree().quit()
