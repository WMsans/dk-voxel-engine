class_name DynamicResolution
extends Node
## Dynamic resolution, the way Donkey Kong Bananza holds 60 fps on Switch 2 (its NdsrUpscaleMinRate /
## NdsrUpscaleMaxRate): the 3D scene renders at a scale between min_scale and max_scale and FSR 1 upscales it to
## the window, so the output stays at the window's resolution. Put it anywhere under the viewport it drives.
##
## A missed frame budget twice in half a second drops the scale a step. After hold_time seconds without that, it
## probes one step back up; a probe that fails right away doubles the wait, so a scene that only just fits does not
## flicker between two scales. Without vsync the frame times show headroom directly, and a step up also needs it.
## Changing the scale costs no hitch in Godot 4.7, so steps can be small.

@export_range(0.25, 1.0, 0.05) var min_scale := 0.7
@export_range(0.25, 1.0, 0.05) var max_scale := 1.0
@export var step := 0.05
@export var target_fps := 60.0
@export var hold_time := 2.0

var _last_usec := 0
var _misses: Array[float] = [] # times of recent missed frames, seconds
var _time := 0.0
var _calm := 0.0 # seconds since the scale last had to drop
var _hold := 2.0
var _raised_at := -100.0
var _avg := 0.0 # smoothed frame time, seconds


func _ready() -> void:
	var vp := get_viewport()
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR
	vp.scaling_3d_scale = max_scale
	_hold = hold_time


func _process(_delta: float) -> void:
	# Real frame times: _process's delta is smoothed, and a miss must not be averaged away.
	var now := Time.get_ticks_usec()
	var dt := (now - _last_usec) / 1e6 if _last_usec else 0.0
	_last_usec = now
	if dt <= 0.0 or dt > 0.5: # first frame, or a stall that is not about rendering (loading, a breakpoint)
		return
	_time += dt
	_avg = lerpf(_avg if _avg > 0.0 else dt, dt, 0.1)
	var budget := 1.0 / target_fps
	var vp := get_viewport()
	var vsync := DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED
	if dt > budget * (1.15 if vsync else 1.02): # under vsync a miss shows as a doubled frame
		_misses.append(_time)
	while not _misses.is_empty() and _time - _misses[0] > 0.5:
		_misses.pop_front()
	if _misses.size() >= 2:
		_misses.clear()
		_calm = 0.0
		if _time - _raised_at < 2.0: # the last step up did not fit: wait longer before the next
			_hold = minf(_hold * 2.0, 30.0)
		vp.scaling_3d_scale = maxf(vp.scaling_3d_scale - step, min_scale)
		return
	_calm += dt
	var headroom := vsync or _avg < budget * 0.88 # under vsync every frame reads as the budget: probe blind
	if _calm >= _hold and headroom and vp.scaling_3d_scale < max_scale:
		vp.scaling_3d_scale = minf(vp.scaling_3d_scale + step, max_scale)
		_raised_at = _time
		_calm = 0.0
	elif _calm >= _hold * 4.0:
		_hold = hold_time # long calm: probe at the normal pace again
