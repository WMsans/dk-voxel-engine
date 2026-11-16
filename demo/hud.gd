extends CanvasLayer
## The HUD node: engine stats, current brush/material/gold and the crosshair. Main points it at the
## world and player and hides it for --shot.

var world: VoxelWorld
var player: CharacterBody3D
var profile := "level"

@onready var status: Label = $Status


func _process(_dt: float) -> void:
	var s := world.get_stats()
	status.text = "%d fps   %.2fM tris   %d chunk meshes   %d colliders   %d chunks   %d queued   %.2f ms/chunk on %d threads\nbrush %.1f m   build: %s   gold: %.1f m³%s\nLMB dig  RMB build  Q smooth  E block  F bomb  wheel size  1-5 material  V fly  R new world  P profile: %s" % [
		Engine.get_frames_per_second(), s.triangles / 1e6, s.meshes, s.bodies, s.chunks, s.pending, s.mesh_ms, s.threads,
		player.brush, world.material_name(player.build_mat), world.gold, "   [FLY]" if player.fly else "", profile]
