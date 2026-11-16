// The demo terrain: today's hills, caves and ore, as a dkv::Generator built out of named rules.
#pragma once
#include "core/voxel_generator.h"

namespace dkv {

// The demo's material ids, in the order demo/main.gd's MATERIALS array lists them. A game using this
// generator should keep its material set in this order; a game with its own generator defines its own ids.
enum DemoMaterial : uint8_t {
	GRASS = 1,
	SOIL = 2,
	ROCK = 3,
	SAND = 4,
	GOLD = 5,
	BEDROCK = 6,
	BRICK = 7,
	CRYSTAL = 8,
};

struct DemoTerrain : Generator {
	uint32_t seed = 7;
	float height = 64.0f; // metres; the peak scale the rules work against
	void generate_block(const int origin[3], float cell, Voxel out[512]) const override;
};

} // namespace dkv
