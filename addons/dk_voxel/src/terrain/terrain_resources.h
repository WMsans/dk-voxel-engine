// The terrain generation profiles shipped with the addon, as VoxelGenerator resources. A game with its own
// terrain adds its generators beside these (or instead of them) and registers them the same way.
#pragma once
#include "core/voxel_resources.h"

// Today's hills, caves and ore.
class DemoTerrainGenerator : public VoxelGenerator {
	GDCLASS(DemoTerrainGenerator, VoxelGenerator)

	uint32_t seed = 7;
	float height = 64.0f;

public:
	std::shared_ptr<const dkv::Generator> create_generator() const override;

	void set_seed(uint32_t p_seed) { seed = p_seed; }
	uint32_t get_seed() const { return seed; }
	void set_height(float p_height) { height = p_height; }
	float get_height() const { return height; }

protected:
	static void _bind_methods();
};

// A hand-made level exported from Blender (tools/blender/dkv_export.py): a .dkvl file of voxelized parts.
// Outside the level is air. Size a bounded VoxelWorld with get_level_size() to hold all of it.
class LevelTerrainGenerator : public VoxelGenerator {
	GDCLASS(LevelTerrainGenerator, VoxelGenerator)

	String level_path;
	mutable std::shared_ptr<const dkv::Generator> loaded; // the last load, for the size and spawn getters

public:
	// Reads the file again every time, so a re-export applies on the next VoxelWorld.reset().
	std::shared_ptr<const dkv::Generator> create_generator() const override;

	void set_level_path(const String &p) { level_path = p; loaded.reset(); }
	String get_level_path() const { return level_path; }
	Vector3 get_level_size() const;
	Vector3 get_spawn_point() const;

protected:
	static void _bind_methods();
};

// Registers the classes above. Call after dk_voxel_register_types().
void dk_voxel_register_terrain();
