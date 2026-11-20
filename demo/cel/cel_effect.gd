## The demo's cel look as one post-transparent compute effect: cel bands, outlines and scalloped leaf edges.
## Add it to the Compositor of the WorldEnvironment or Camera3D. The scene must light the cel way (Lambert, no
## specular, flat ambient, hard shadows) and mark leaves in roughness (voxel_terrain_cel.gdshader).
##
## Two steps a frame: a half-resolution jump flood over leaf pixels (leaf_seed.glsl, whose first pass also
## snapshots the lit image), then cel_outline.glsl, which reads the snapshot and writes the result back. Compute on the colour buffer instead of full-screen quads with
## hint_screen_texture: the screen texture comes with a blurred mip chain this look never reads, and that
## chain cost more than everything else here together.
##
## Not a @tool: one instance serves every viewport it renders, and the editor's differently sized viewports
## would make it free textures still in use. So the look shows in the running game, not the editor viewport.
class_name CelEffect
extends CompositorEffect

## Jump sizes in half-resolution cells, JFA+1. Reach is their sum (16 cells, 32 px), so leaf_bump_px must stay
## below it.
const STEPS: Array[int] = [8, 4, 2, 1, 1]

## Luminance (x) to banded colour. Keep its gradient on constant interpolation for hard steps; tint the dark
## stops for coloured shadows.
@export var ramp: Texture2D = preload("res://demo/cel/cel_ramp.tres")
@export var outline_color := Color(0.05, 0.05, 0.08, 1.0)
## Line width in screen pixels.
@export var thickness := 1.5
## Depth gap, in metres, below which overlapping surfaces get no line.
@export var depth_threshold := 0.5
## Chromaticity distance that draws a line on the nearer surface with no depth gap.
@export var color_threshold := 0.08
## Leaves grow their scallops only over surfaces at least this many metres behind them, so lobes of one clump
## don't scallop over each other. Keep it at least depth_threshold so every scalloped edge gets its line.
@export var depth_gap := 2.0

var _rd: RenderingDevice
var _seed_shader: RID
var _seed_pipeline: RID
var _cel_shader: RID
var _cel_pipeline: RID
var _sampler: RID
var _seed: Array[RID] = [] # jump-flood ping-pong pair
var _copy: RID # snapshot of the lit image, read while the colour buffer is written
var _size := Vector2i.ZERO
var _format := -1
var _bump_px: float = ProjectSettings.get_setting("shader_globals/leaf_bump_px", {"value": 20.0}).value


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	needs_normal_roughness = true
	RenderingServer.call_on_render_thread(_init_rd)


func _init_rd() -> void:
	_rd = RenderingServer.get_rendering_device()
	_seed_shader = _rd.shader_create_from_spirv(load("res://demo/cel/leaf_seed.glsl").get_spirv())
	_seed_pipeline = _rd.compute_pipeline_create(_seed_shader)
	_cel_shader = _rd.shader_create_from_spirv(load("res://demo/cel/cel_outline.glsl").get_spirv())
	_cel_pipeline = _rd.compute_pipeline_create(_cel_shader)
	_sampler = _rd.sampler_create(RDSamplerState.new()) # nearest: every read is a texelFetch


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or not _rd:
		return
	for rid in _seed + [_copy, _sampler, _seed_shader, _cel_shader]: # freeing a shader frees its pipeline
		if rid.is_valid():
			_rd.free_rid(rid)


func _render_callback(type: int, render_data: RenderData) -> void:
	if not _rd or type != effect_callback_type:
		return
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if not buffers:
		return
	var size := buffers.get_internal_size()
	if size.x == 0 or size.y == 0:
		return
	var color := buffers.get_color_layer(0)
	var format := _rd.texture_get_format(color).format
	if size != _size or format != _format:
		_make_textures(size, format)
	var nr := buffers.get_texture(&"forward_clustered", &"normal_roughness")
	var depth := buffers.get_depth_layer(0)
	var proj := render_data.get_render_scene_data().get_cam_projection()
	# The reverse-Z projection the depth buffer was written with: view distance = W.z / (depth + Z.z).
	var groups := Vector3i(ceili(size.x / 8.0), ceili(size.y / 8.0), 1)

	# Jump flood: step 0 (full resolution) snapshots and seeds, the rest jump over half-resolution cells.
	var floats := PackedFloat32Array([proj.w.z, proj.z.z, depth_gap, 0.0]).to_byte_array()
	var cell_groups := Vector3i(ceili(size.x / 16.0), ceili(size.y / 16.0), 1)
	for i in STEPS.size() + 1:
		var pc := PackedInt32Array([size.x, size.y, 0 if i == 0 else STEPS[i - 1], 0]).to_byte_array() + floats
		_dispatch(_seed_shader, _seed_pipeline, groups if i == 0 else cell_groups, pc, [
			_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, [_sampler, nr]),
			_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 1, [_seed[(i + 1) % 2]]),
			_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 2, [_seed[i % 2]]),
			_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, [_sampler, depth]),
			_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 4, [color]),
			_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 5, [_copy]),
		])

	# Cel + outline, from the snapshot into the colour buffer.
	var line := outline_color.srgb_to_linear()
	var pc := PackedFloat32Array([line.r, line.g, line.b, line.a]).to_byte_array()
	pc += PackedInt32Array([size.x, size.y]).to_byte_array()
	pc += PackedFloat32Array([thickness, depth_threshold, color_threshold, _bump_px, proj.w.z, proj.z.z]).to_byte_array()
	_dispatch(_cel_shader, _cel_pipeline, groups, pc, [
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, [_sampler, _copy]),
		_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 1, [color]),
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, [_sampler, depth]),
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, [_sampler, nr]),
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, [_sampler, _seed[STEPS.size() % 2]]),
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, [_sampler, RenderingServer.texture_get_rd_texture(ramp.get_rid())]),
	])


func _dispatch(shader: RID, pipeline: RID, groups: Vector3i, pc: PackedByteArray, uniforms: Array) -> void:
	var list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(list, pipeline)
	_rd.compute_list_bind_uniform_set(list, UniformSetCacheRD.get_cache(shader, 0, uniforms), 0)
	_rd.compute_list_set_push_constant(list, pc, pc.size())
	_rd.compute_list_dispatch(list, groups.x, groups.y, groups.z)
	_rd.compute_list_end()


func _make_textures(size: Vector2i, format: int) -> void:
	_free_textures()
	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R32_UINT # two packed seed offsets a half-resolution cell, see leaf_seed.glsl
	fmt.width = (size.x + 1) / 2
	fmt.height = (size.y + 1) / 2
	fmt.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	_seed = [_rd.texture_create(fmt, RDTextureView.new()), _rd.texture_create(fmt, RDTextureView.new())]
	fmt.format = format
	fmt.width = size.x
	fmt.height = size.y
	_copy = _rd.texture_create(fmt, RDTextureView.new())
	_size = size
	_format = format


func _free_textures() -> void:
	for rid in _seed + [_copy]:
		if rid.is_valid():
			_rd.free_rid(rid)
	_seed = []
	_copy = RID()


func _uniform(type: int, binding: int, ids: Array) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = type
	u.binding = binding
	for id in ids:
		u.add_id(id)
	return u
