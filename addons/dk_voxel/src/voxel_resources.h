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

// One entry of a VoxelMaterialSet. hardness: erase power needed, 255 = unbreakable.
class VoxelMaterialDef : public Resource {
	GDCLASS(VoxelMaterialDef, Resource)

	String name;
	Color color_a = Color(0.5f, 0.5f, 0.5f);
	Color color_b = Color(0.6f, 0.6f, 0.6f);
	float pattern_scale = 0.3f;
	float roughness = 0.9f;
	float metallic = 0.0f;
	float emission = 0.0f;
	int hardness = 0;

public:
	void set_name(const String &p_name) { name = p_name; }
	String get_name() const { return name; }
	void set_color_a(const Color &p_color) { color_a = p_color; }
	Color get_color_a() const { return color_a; }
	void set_color_b(const Color &p_color) { color_b = p_color; }
	Color get_color_b() const { return color_b; }
	void set_pattern_scale(float p_scale) { pattern_scale = p_scale; }
	float get_pattern_scale() const { return pattern_scale; }
	void set_roughness(float p_roughness) { roughness = p_roughness; }
	float get_roughness() const { return roughness; }
	void set_metallic(float p_metallic) { metallic = p_metallic; }
	float get_metallic() const { return metallic; }
	void set_emission(float p_emission) { emission = p_emission; }
	float get_emission() const { return emission; }
	void set_hardness(int p_hardness) { hardness = CLAMP(p_hardness, 0, 255); }
	int get_hardness() const { return hardness; }

protected:
	static void _bind_methods();
};

// Index = material id. Missing ids read as entry 0.
class VoxelMaterialSet : public Resource {
	GDCLASS(VoxelMaterialSet, Resource)

	TypedArray<VoxelMaterialDef> materials;

public:
	void set_materials(const TypedArray<VoxelMaterialDef> &p_materials) { materials = p_materials; }
	TypedArray<VoxelMaterialDef> get_materials() const { return materials; }

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
