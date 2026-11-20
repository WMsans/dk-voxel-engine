// A sampled signed-distance grid, and the voxelizer that turns a triangle mesh into one. The level parts
// (terrain/level_terrain.h) are these grids made offline; VoxelWorld::build_mesh makes one at load time and
// pastes it as an edit. Godot-free.
#pragma once
#include "voxel_types.h"

#include <vector>

namespace dkv {

struct SdfGrid {
	static constexpr float SD_SCALE = 16.0f; // stored value = signed distance in cells * SD_SCALE
	float origin[3] = {0, 0, 0}, cell = 1.0f; // position of sample (0,0,0), spacing; metres
	int n[3] = {0, 0, 0}; // samples per axis, each >= 2
	std::vector<int8_t> sd; // x fastest, then y, then z; negative inside, clamped to +-127
	float sample(Vec3 p) const; // trilinear, metres; outside the grid = as far as the grid stores
};

// `tris` = 3 corners per triangle, metres. The mesh may be several closed shells that overlap (a canopy of
// leaf clusters): a point is inside when at least two of three axis rays see a non-zero winding number.
// Samples sit on the multiples of `cell`, so a grid made at a volume's cell size lines up with its voxels.
// Distances are exact within 2 cells of the surface, which is all the density band needs.
SdfGrid voxelize_triangles(const std::vector<Vec3> &tris, float cell);

} // namespace dkv
