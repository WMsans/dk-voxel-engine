// Godot-facing resources: terrain generators and the material set that feeds the terrain shader.
#pragma once
#include "core/voxel_volume.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <memory>

using namespace godot;

// Node group every VoxelViewer joins on entering the tree. VoxelWorld streams for the viewers in this
// group that share its World3D.
inline constexpr const char *VIEWER_GROUP = "dk_voxel_viewers";

// Base class for terrain generation. Abstract: games subclass it in C++ and return their own dkv::Generator.
// VoxelWorld calls create_generator() on start and on every reset(), so property changes apply on the next reset.
class VoxelGenerator : public Resource {
	GDCLASS(VoxelGenerator, Resource)

public:
	virtual std::shared_ptr<const dkv::Generator> create_generator() const = 0;

protected:
	static void _bind_methods() {}
};

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

// Drives chunk streaming, LOD and collision for the VoxelWorld nodes sharing its World3D.
class VoxelViewer : public Node3D {
	GDCLASS(VoxelViewer, Node3D)

	float view_distance = 160.0f;
	float vertical_view_distance = 96.0f;
	float collision_distance = 40.0f;
	bool enabled = true;

protected:
	static void _bind_methods();

public:
	void _ready() override;

	void set_view_distance(float v) { view_distance = v; }
	float get_view_distance() const { return view_distance; }
	void set_vertical_view_distance(float v) { vertical_view_distance = v; }
	float get_vertical_view_distance() const { return vertical_view_distance; }
	void set_collision_distance(float v) { collision_distance = v; }
	float get_collision_distance() const { return collision_distance; }
	void set_enabled(bool v) { enabled = v; }
	bool get_enabled() const { return enabled; }
};
