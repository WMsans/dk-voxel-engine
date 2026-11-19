// A hand-made level, built the way Donkey Kong Bananza builds its stages: artist meshes are voxelized
// offline into signed-distance grids (one per part, one material each), and the level is an ordered list of
// operations on those parts — paste, erase, repaint — composed at generation time. The grids come from
// tools/blender/dkv_export.py. Godot-free: it runs on worker threads and in the self-test.
//
// .dkvl file, little-endian, Godot axes (y up), metres:
//   char[4] "DKVL", u32 version = 1
//   f32 size[3]     the level spans [0, size]; a bounded VoxelWorld this big holds all of it
//   f32 spawn[3]    where the player starts
//   u32 part_count, then per part, in the order they apply:
//     u8 mode (0 paste, 1 erase, 2 paint), u8 material, u16 reserved
//     f32 origin[3]  position of grid sample (0,0,0)
//     f32 cell       grid spacing
//     u32 n[3]       samples per axis, each >= 2
//     i8 sd[n.x * n.y * n.z]  x fastest, then y, then z; signed distance in cell / SD_SCALE units,
//                    negative inside, clamped to +-127 (so about +-8 cells: enough for the +-1 cell band)
#pragma once
#include "core/sdf_grid.h"
#include "core/voxel_generator.h"

#include <string>
#include <vector>

namespace dkv {

struct LevelTerrain : Generator {
	enum Mode : uint8_t { PASTE, ERASE, PAINT };
	static constexpr float SD_SCALE = SdfGrid::SD_SCALE;

	struct Part : SdfGrid {
		uint8_t mode = PASTE, material = 0;
	};

	float size[3] = {0, 0, 0}, spawn[3] = {0, 0, 0};
	std::vector<Part> parts;

	// Replaces the level with the file's contents. On bad data returns false, says why in `err`, and leaves
	// the level unchanged.
	bool load(const uint8_t *data, size_t len, std::string &err);
	void generate_block(const int origin[3], float cell, Voxel out[512]) const override;
};

} // namespace dkv
