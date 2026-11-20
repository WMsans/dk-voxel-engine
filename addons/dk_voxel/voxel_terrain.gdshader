// Terrain material for VoxelWorld meshes.
// COLOR.r / .g = the two material ids of the triangle (constant across it), .b = blend, .a = baked AO.
// Albedo and normal come from one Texture2DArray each, layer = material id, triplanar-projected in world
// space, so nothing stretches on cliffs or cave ceilings, no UVs are needed, and the projection runs on
// across chunk borders (mesh vertices are chunk-relative, so VERTEX alone would restart the texture at
// every chunk). The roughness and AO maps are projected the same way: roughness multiplies the material's
// scalar roughness, AO multiplies the baked per-vertex AO in COLOR.a.
// The two colours are a tint ramp over the albedo: set both to white to show a texture unmodified.
shader_type spatial;

#include "res://addons/dk_voxel/voxel_terrain.gdshaderinc"
