## Screen-space cel-shading pass. Add this node anywhere in a 3D scene.
##
## Bands the lit image by luminance through [member ramp]. Needs
## view-independent lighting (no specular or sky reflections, Lambert diffuse)
## or bands flicker as the camera moves. Use flat neutral ambient (not sky) and
## hard shadow filtering, or bands get muddy hue shifts and speckled edges. Runs just before [OutlinePostProcess], so lines stay on top.
class_name CelShadingPostProcess
extends Node3D

## Luminance (x) to banded colour. Keep its gradient on constant interpolation
## for hard steps; tint the dark stops for coloured shadows.
@export var ramp: Texture2D = preload("res://demo/cel/cel_ramp.tres"):
	set(value):
		ramp = value
		_set_param("ramp", value)

var _material: ShaderMaterial


func _ready() -> void:
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(2.0, 2.0)
	quad.mesh = mesh
	quad.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	quad.extra_cull_margin = 16384.0
	_material = ShaderMaterial.new()
	_material.shader = preload("res://demo/cel/cel_shading.gdshader")
	# ponytail: reads the opaque-only screen copy, so any transparent object drawn
	# before this (priority < 126) is painted over. Move to a CompositorEffect if
	# the scene gets real transparents.
	_material.render_priority = 126
	quad.material_override = _material
	add_child(quad)
	_set_param("ramp", ramp)


func _set_param(param: StringName, value: Variant) -> void:
	if _material:
		_material.set_shader_parameter(param, value)
