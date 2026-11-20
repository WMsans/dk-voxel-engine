extends RefCounted
## Shared helpers for the benchmark scripts: timing, percentiles, waiting on the stream, reporting.
## Results print as a table and are written as JSON next to the scripts in benchmarks/results/.

var tree: SceneTree
var name: String
var results := {}
var _order: Array[String] = []


func _init(t: SceneTree, bench_name: String) -> void:
	tree = t
	name = bench_name


static func now_ms() -> float:
	return Time.get_ticks_usec() / 1000.0


static func percentile(samples: PackedFloat64Array, p: float) -> float:
	if samples.is_empty():
		return 0.0
	var s := samples.duplicate()
	s.sort()
	return s[clampi(int(round(p / 100.0 * (s.size() - 1))), 0, s.size() - 1)]


static func mean(samples: PackedFloat64Array) -> float:
	if samples.is_empty():
		return 0.0
	var t := 0.0
	for v in samples:
		t += v
	return t / samples.size()


## Waits until the world has nothing queued for `quiet` frames in a row. Returns elapsed ms, or -1 on timeout.
func await_idle(w: VoxelWorld, timeout_ms := 120000.0, quiet := 3) -> float:
	var t0 := now_ms()
	var q := 0
	while now_ms() - t0 < timeout_ms:
		await tree.process_frame
		if w.get_pending_count() == 0:
			q += 1
			if q >= quiet:
				return now_ms() - t0
		else:
			q = 0
	return -1.0


## Records one result under `key`. `value` is a Dictionary of metric -> number (or string).
func record(key: String, value: Dictionary) -> void:
	if not results.has(key):
		_order.append(key)
	results[key] = value
	var parts: PackedStringArray = []
	for k in value:
		var v = value[k]
		parts.append("%s=%s" % [k, ("%.3f" % v) if v is float else str(v)])
	print("  %-34s %s" % [key, "  ".join(parts)])


static func system_info() -> Dictionary:
	return {
		"godot": Engine.get_version_info().string,
		"os": OS.get_name() + " " + OS.get_version(),
		"cpu": OS.get_processor_name(),
		"cores": OS.get_processor_count(),
		"gpu": RenderingServer.get_video_adapter_name(),
		"gpu_vendor": RenderingServer.get_video_adapter_vendor(),
		"renderer": RenderingServer.get_current_rendering_method(),
		"display": DisplayServer.get_name(),
		"debug_build": OS.is_debug_build(),
		"date": Time.get_datetime_string_from_system(),
	}


func print_header() -> void:
	print("\n=== %s ===" % name)
	var info := system_info()
	for k in info:
		print("  %-12s %s" % [k, info[k]])
	print("")


## Writes benchmarks/results/<name>_<timestamp>.json and returns its path.
func save() -> String:
	var dir := ProjectSettings.globalize_path("res://benchmarks/results")
	DirAccess.make_dir_recursive_absolute(dir)
	var stamp := Time.get_datetime_string_from_system().replace(":", "-")
	var path := dir.path_join("%s_%s.json" % [name, stamp])
	var ordered := {}
	for k in _order:
		ordered[k] = results[k]
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(JSON.stringify({"system": system_info(), "results": ordered}, "  ", false))
	f.close()
	print("\nresults written to ", path)
	return path
