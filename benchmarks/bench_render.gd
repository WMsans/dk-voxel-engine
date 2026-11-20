extends SceneTree
## Rendering benchmark: runs the real demo scene (demo/main.tscn) in a window and measures frame times.
##   godot --path . -s benchmarks/bench_render.gd
##   godot --path . -s benchmarks/bench_render.gd -- --profile=level --frames=150 --resolution=1280x720
## For each terrain profile it waits for the demo's spawn, then renders fixed views under three settings:
##   full     the demo as shipped (the CelEffect compositor pass, SSAO, SSIL, glow, shadows)
##   no_post  without the CelEffect pass
##   minimal  no_post, and also without SSAO, SSIL, glow and sun shadows (each only if the scene enables it)
## then flies an orbit around the spawn (streaming while rendering), then plays: walks while digging and
## bombing, and sprints in fly mode (see play()). VSync is off and max_fps is 0.
## GPU and render-CPU times come from the RenderingServer's viewport measurements.
## Writes benchmarks/results/render_<time>.json.

const Lib := preload("res://benchmarks/bench_lib.gd")
const MAIN := preload("res://demo/main.tscn")

var lib: Lib
var frames_per := 300
var vp_rid: RID


func _initialize() -> void:
	var profiles := ["level", "procedural"]
	var res := Vector2i(1920, 1080)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--profile="):
			profiles = [a.trim_prefix("--profile=")]
		elif a.begins_with("--frames="):
			frames_per = int(a.trim_prefix("--frames="))
		elif a.begins_with("--resolution="):
			var wh := a.trim_prefix("--resolution=").split("x")
			res = Vector2i(int(wh[0]), int(wh[1]))
	DisplayServer.window_set_size(res) # a tiling window manager may override this; the size used is reported
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	lib = Lib.new(self, "render")
	lib.print_header()
	vp_rid = root.get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(vp_rid, true)
	await process_frame
	print("  resolution   %s\n" % DisplayServer.window_get_size())
	for p in profiles:
		await run_profile(p)
	var size := DisplayServer.window_get_size()
	lib.results["resolution"] = {"size": str(size), "megapixels": size.x * size.y / 1e6}
	lib.save()
	quit(0)


func run_profile(profile: String) -> void:
	print("-- profile: %s --" % profile)
	var main := MAIN.instantiate()
	main.profile = profile
	var t0 := Lib.now_ms()
	root.add_child(main)
	while main.player == null: # during _initialize the scene's _ready runs on the first frame
		await process_frame
	var player: Node3D = main.player
	while not player.active and Lib.now_ms() - t0 < 60000.0:
		await process_frame
	lib.record("%s startup" % profile, {"spawn_ms": Lib.now_ms() - t0, "player_active": player.active})
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	player.fly = true
	var world: VoxelWorld = main.world
	var cam: Camera3D = main.player.cam

	for i in views(main, profile).size():
		var view: Array = views(main, profile)[i]
		await place(world, player, cam, view[0], view[1])
		for setting in ["full", "no_post", "minimal"]:
			apply_setting(main, setting)
			await measure("%s view%d %s" % [profile, i, setting], world)
		apply_setting(main, "full")

	# Orbit: streaming + rendering together, the closest thing to play.
	var centre: Vector3 = main.spawn_position()
	var times := PackedFloat64Array()
	var n := frames_per * 2
	var last := Lib.now_ms()
	for f in n:
		var a := TAU * f / n
		player.global_position = centre + Vector3(cos(a) * 45.0, 28.0, sin(a) * 45.0) - cam.position
		cam.look_at(centre + Vector3.UP * 4.0)
		await process_frame
		var t := Lib.now_ms()
		times.append(t - last)
		last = t
	var s := world.get_stats()
	lib.record("%s orbit" % profile, {"frames": n, "fps_avg": 1000.0 / Lib.mean(times), "frame_p50_ms": Lib.percentile(times, 50),
			"frame_p95_ms": Lib.percentile(times, 95), "frame_p99_ms": Lib.percentile(times, 99), "frame_max_ms": Lib.percentile(times, 100),
			"triangles": s.triangles, "meshes": s.meshes, "render_scale": root.scaling_3d_scale})
	await play(main, profile)
	main.free()
	for i in 5:
		await process_frame


## Play: what a player does, under the full settings. Walks (real input actions, physics body) while digging
## at the tools' own rate and throwing a bomb every 1.5 s, so remeshing, collision rebuilds, debris and bombs
## all land in the measured frames; then sprints in fly mode (streaming at ~28 m/s).
func play(main: Node, profile: String) -> void:
	var player: FlyWalkController = main.player
	var cam: Camera3D = player.cam
	var tools: VoxelTools = main.tools
	var world: VoxelWorld = main.world
	player.fly = false
	player.global_position = VoxelUtil.ground(world, main.spawn_position()) + Vector3.UP * 2.0
	player.velocity = Vector3.ZERO
	cam.rotation = Vector3(-0.45, 0, 0) # looking at the ground a few metres ahead
	await create_timer(0.5).timeout
	await lib.await_idle(world, 30000.0, 10)
	for phase in ["walk+dig", "fly"]:
		var fly: bool = phase == "fly"
		player.fly = fly
		cam.rotation.x = -0.1 if fly else -0.45
		Input.action_press(&"ui_up")
		var times := PackedFloat64Array()
		var t_end := Lib.now_ms() + 10000.0
		var next_dig := 0.0
		var next_bomb := 500.0
		var start := Lib.now_ms()
		var last := start
		while Lib.now_ms() < t_end:
			var now := Lib.now_ms() - start
			player.rotate_y((0.35 if profile == "level" else 0.08) * (Lib.now_ms() - last) / 1000.0) # level: circle inside the bounds
			if not fly and now >= next_dig: # the tools' LMB path, at their 0.07 s cooldown
				next_dig = now + 70.0
				var hit := world.raycast(cam.global_position, -cam.global_basis.z, tools.reach)
				if not hit.is_empty():
					tools.dug.emit(world.dig_sphere(hit.position - hit.normal * tools.brush * 0.3, tools.brush, tools.dig_power), hit.position, 4.0, tools.brush)
			if not fly and now >= next_bomb:
				next_bomb = now + 1500.0
				tools.throw_bomb()
			await process_frame
			var t := Lib.now_ms()
			times.append(t - last)
			last = t
		Input.action_release(&"ui_up")
		var over := 0
		for f in times:
			if f > 1000.0 / 60.0:
				over += 1
		var s := world.get_stats()
		lib.record("%s play %s" % [profile, phase], {"frames": times.size(), "fps_avg": 1000.0 / Lib.mean(times),
				"frame_p50_ms": Lib.percentile(times, 50), "frame_p99_ms": Lib.percentile(times, 99),
				"frame_max_ms": Lib.percentile(times, 100), "frames_over_16.7ms": over, "triangles": s.triangles, "bodies": s.bodies,
				"render_scale": root.scaling_3d_scale})


## [camera position, look-at] pairs, the demo's own beauty-shot views.
func views(main: Node, profile: String) -> Array:
	if profile == "level":
		var c: Vector3 = main.level.get_level_size() * 0.5
		var spawn: Vector3 = main.level.get_spawn_point()
		return [[c + Vector3(-45, 35, 55), c], [spawn + Vector3(0, 4, 6), c + Vector3(0, 2, 0)],
				[spawn + main.LEVEL_TREE + Vector3(6, 6, 22), spawn + main.LEVEL_TREE + Vector3(0, 7, 0)]]
	var world: VoxelWorld = main.world
	var tower := world.raycast(Vector3(14, 400, -6), Vector3.DOWN, 600.0).get("position", Vector3(14, 20, -6)) as Vector3
	var grove := world.raycast(Vector3(-14, 400, 10), Vector3.DOWN, 600.0).get("position", Vector3(-14, 20, 10)) as Vector3
	return [[tower + Vector3(-20, 14, 30), tower + Vector3(-6, 3, 2)], [grove + Vector3(26, 12, 22), grove + Vector3(0, 8, 0)],
			[Vector3(-70.0, 60.0, 85.0), Vector3(0, 20, 0)]]


## Moves the player body (its VoxelViewer streams the view in) and waits for the stream to settle.
func place(world: VoxelWorld, player: Node3D, cam: Camera3D, at: Vector3, target: Vector3) -> void:
	player.global_position = at - cam.position
	cam.look_at(target)
	await create_timer(0.3).timeout
	await lib.await_idle(world, 30000.0, 10)


## Turns features off for a setting; "full" restores what the scene itself enables (as first seen here).
func apply_setting(main: Node, setting: String) -> void:
	var post := setting == "full"
	var fx := setting != "minimal"
	var we := main.get_node("WorldEnvironment") as WorldEnvironment
	var env := we.environment
	var sun := main.get_node("Sun") as DirectionalLight3D
	if not main.has_meta(&"bench_scene"):
		main.set_meta(&"bench_scene", [env.ssao_enabled, env.ssil_enabled, env.glow_enabled, sun.shadow_enabled])
	var scene: Array = main.get_meta(&"bench_scene")
	for e: CompositorEffect in we.compositor.compositor_effects:
		e.enabled = post
	env.ssao_enabled = fx and scene[0]
	env.ssil_enabled = fx and scene[1]
	env.glow_enabled = fx and scene[2]
	sun.shadow_enabled = fx and scene[3]


func measure(key: String, world: VoxelWorld) -> void:
	for i in 20: # warm-up: shader compiles and pipeline changes after a setting switch
		await process_frame
	var times := PackedFloat64Array()
	var gpu := PackedFloat64Array()
	var cpu := PackedFloat64Array()
	var last := Lib.now_ms()
	for f in frames_per:
		await process_frame
		var t := Lib.now_ms()
		times.append(t - last)
		last = t
		gpu.append(RenderingServer.viewport_get_measured_render_time_gpu(vp_rid))
		cpu.append(RenderingServer.viewport_get_measured_render_time_cpu(vp_rid))
	var s := world.get_stats()
	lib.record(key, {"fps_avg": 1000.0 / Lib.mean(times), "frame_p50_ms": Lib.percentile(times, 50),
			"frame_p99_ms": Lib.percentile(times, 99), "gpu_ms": Lib.mean(gpu), "render_cpu_ms": Lib.mean(cpu),
			"triangles": s.triangles, "render_scale": root.scaling_3d_scale, "draw_calls": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME),
			"primitives": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)})
