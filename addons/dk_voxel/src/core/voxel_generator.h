// The interface a game's terrain generator implements. Godot-free: it runs on worker threads.
#pragma once
#include "voxel_types.h"

namespace dkv {

struct Generator {
	virtual ~Generator() = default;
	// origin = the block's cell coordinates; index = vidx(x & 7, y & 7, z & 7). Must be const, thread-safe
	// and deterministic in (settings, origin): an unloaded chunk is regenerated exactly.
	virtual void generate_block(const int origin[3], float cell, Voxel out[512]) const = 0;
};

} // namespace dkv
