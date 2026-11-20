## Jump-flood pass: for every pixel, the nearest leaf pixel within 31 px that is
## in front of it by more than [member depth_gap].
##
## Runs after the opaque pass and publishes the result as the global shader
## uniform [code]leaf_seed[/code] (RG = pixel coordinates), which
## leaf_scallop.gdshaderinc reads to grow scalloped leaf edges. Add it to the
## Compositor of the WorldEnvironment or Camera3D.
##
## Not a @tool: one instance serves every viewport it renders, and the editor's
## differently sized viewports would make it free a texture still in use. So the
## scallops show in the running game, not the editor viewport.
class_name LeafSeedEffect
extends CompositorEffect

## Jump sizes, JFA+1. Reach is their sum (31 px), so leaf_bump_px must stay below
## it. An even count leaves the result in _tex[0].
const STEPS: Array[int] = [16, 8, 4, 2, 1, 1]

## Leaves grow their scallops only over surfaces at least this many metres behind
## them, so lobes of one clump don't scallop over each other. Keep it equal to
## OutlinePostProcess.depth_threshold so every scalloped edge gets its line.
@export var depth_gap := 2.0

var _rd: RenderingDevice
var _shader: RID
var _pipeline: RID
var _sampler: RID
var _tex: Array[RID] = []
# Previous textures, kept one resize longer: leaf_seed may still point at them.
var _old: Array[RID] = []
var _size := Vector2i.ZERO
var _seed := Texture2DRD.new()


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE
	needs_normal_roughness = true
	RenderingServer.global_shader_parameter_set(&"leaf_seed", _seed)
	RenderingServer.call_on_render_thread(_init_rd)


func _init_rd() -> void:
	_rd = RenderingServer.get_rendering_device()
	var spirv: RDShaderSPIRV = load("res://demo/cel/leaf_seed.glsl").get_spirv()
	_shader = _rd.shader_create_from_spirv(spirv)
	_pipeline = _rd.compute_pipeline_create(_shader)
	_sampler = _rd.sampler_create(RDSamplerState.new())


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	# Unpublish first so no material keeps sampling the textures freed below.
	# ponytail: assumes one LeafSeedEffect at a time.
	RenderingServer.global_shader_parameter_set(&"leaf_seed", null)
	_seed.texture_rd_rid = RID()
	if _rd:
		for rid in _tex + _old:
			_rd.free_rid(rid)
		if _sampler.is_valid():
			_rd.free_rid(_sampler)
		if _shader.is_valid():
			_rd.free_rid(_shader) # frees the pipeline too


func _render_callback(type: int, render_data: RenderData) -> void:
	if not _rd or type != effect_callback_type:
		return
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if not buffers:
		return
	var size := buffers.get_internal_size()
	if size.x == 0 or size.y == 0:
		return
	if size != _size:
		_make_textures(size)
	var nr := buffers.get_texture(&"forward_clustered", &"normal_roughness")
	var depth := buffers.get_depth_texture()
	var proj := render_data.get_render_scene_data().get_cam_projection()
	# This is the reverse-Z projection the depth buffer was written with: depth
	# d = (Z.z * zv + W.z) / -zv, so view distance = W.z / (d + Z.z).
	var floats := PackedFloat32Array([proj.w.z, proj.z.z, depth_gap, 0.0]).to_byte_array()
	var groups := Vector3i(ceili(size.x / 8.0), ceili(size.y / 8.0), 1)
	for i in STEPS.size() + 1:
		var pc := PackedInt32Array([size.x, size.y, 0 if i == 0 else STEPS[i - 1], 0]).to_byte_array() + floats
		var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, [
			_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, [_sampler, nr]),
			_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 1, [_tex[(i + 1) % 2]]),
			_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 2, [_tex[i % 2]]),
			_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, [_sampler, depth]),
		])
		var list := _rd.compute_list_begin()
		_rd.compute_list_bind_compute_pipeline(list, _pipeline)
		_rd.compute_list_bind_uniform_set(list, uniform_set, 0)
		_rd.compute_list_set_push_constant(list, pc, pc.size())
		_rd.compute_list_dispatch(list, groups.x, groups.y, groups.z)
		_rd.compute_list_end()


func _make_textures(size: Vector2i) -> void:
	for rid in _old:
		_rd.free_rid(rid)
	_old = _tex
	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT
	fmt.width = size.x
	fmt.height = size.y
	fmt.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	_tex = [_rd.texture_create(fmt, RDTextureView.new()), _rd.texture_create(fmt, RDTextureView.new())]
	_size = size
	# Texture2DRD wraps an RD texture for the shaders; update it on the main thread.
	_seed.set_deferred(&"texture_rd_rid", _tex[0])


func _uniform(type: int, binding: int, ids: Array) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = type
	u.binding = binding
	for id in ids:
		u.add_id(id)
	return u
