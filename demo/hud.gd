extends CanvasLayer
## Engine stats and the crosshair. `world` is set in the scene; the game writes its own lines to `info`.

@export var world: VoxelWorld

var info := ""

@onready var status: Label = $Status


func _process(_dt: float) -> void:
	var s := world.get_stats()
	status.text = "%d fps   %.2fM tris   %d chunk meshes   %d colliders   %d chunks   %d queued   %.2f ms/chunk on %d threads\n%s" % [
		Engine.get_frames_per_second(), s.triangles / 1e6, s.meshes, s.bodies, s.chunks, s.pending, s.mesh_ms, s.threads, info]
