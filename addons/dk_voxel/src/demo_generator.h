// The demo terrain generator: today's hills, caves and ore, as a dkv::Generator.
#pragma once
#include "voxel_core.h"

namespace dkv {

struct DemoTerrain : Generator {
	uint32_t seed = 7;
	float height = 64.0f; // metres; plays the role of today's H = ny * cell
	void generate_block(const int origin[3], float cell, Voxel out[512]) const override;
};

} // namespace dkv
