## Screen-space outline pass. Add this node anywhere in a 3D scene.
##
## Lines are drawn where the view-space depth jumps by more than
## [member depth_threshold] metres. Two objects overlapping on screen whose
## surfaces are closer together than that get no line between them, unless
## their hues differ by more than [member color_threshold] (leaves over trunk):
## then the nearer one gets the line.
class_name OutlinePostProcess
extends Node3D

@export var outline_color := Color(0.05, 0.05, 0.08, 1.0):
	set(value):
		outline_color = value
		_set_param("outline_color", value)

## Line width in screen pixels.
@export var thickness := 1.5:
	set(value):
		thickness = value
		_set_param("thickness", value)

## Depth gap, in metres, below which overlapping surfaces get no line.
@export var depth_threshold := 0.5:
	set(value):
		depth_threshold = value
		_set_param("depth_threshold", value)

## Chromaticity distance that draws a line on the nearer surface with no depth gap.
@export var color_threshold := 0.08:
	set(value):
		color_threshold = value
		_set_param("color_threshold", value)

var _material: ShaderMaterial


func _ready() -> void:
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(2.0, 2.0)
	quad.mesh = mesh
	quad.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	# The quad's clip position is overridden in the shader, so its own bounds mean
	# nothing; keep it from being culled when the camera looks away from this node.
	quad.extra_cull_margin = 16384.0
	_material = ShaderMaterial.new()
	_material.shader = preload("res://demo/cel/outline.gdshader")
	_material.render_priority = 127
	quad.material_override = _material
	add_child(quad)
	_set_param("outline_color", outline_color)
	_set_param("thickness", thickness)
	_set_param("depth_threshold", depth_threshold)
	_set_param("color_threshold", color_threshold)


func _set_param(param: StringName, value: Variant) -> void:
	if _material:
		_material.set_shader_parameter(param, value)
