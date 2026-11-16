// Godot-facing resources every game shares: the generator base class and the viewer. Concrete generators
// (terrain profiles) live in ../terrain/.
#pragma once
#include "voxel_volume.h"

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
