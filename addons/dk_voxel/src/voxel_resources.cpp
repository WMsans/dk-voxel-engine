#include "voxel_resources.h"

#include "demo_terrain.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

std::shared_ptr<const dkv::Generator> DemoTerrainGenerator::create_generator() const {
	auto g = std::make_shared<dkv::DemoTerrain>();
	g->seed = seed;
	g->height = height;
	return g;
}

void DemoTerrainGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_seed", "v"), &DemoTerrainGenerator::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &DemoTerrainGenerator::get_seed);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ClassDB::bind_method(D_METHOD("set_height", "v"), &DemoTerrainGenerator::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &DemoTerrainGenerator::get_height);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height"), "set_height", "get_height");
}

void VoxelViewer::_ready() {
	add_to_group(VIEWER_GROUP);
}

void VoxelViewer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_view_distance", "v"), &VoxelViewer::set_view_distance);
	ClassDB::bind_method(D_METHOD("get_view_distance"), &VoxelViewer::get_view_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "view_distance"), "set_view_distance", "get_view_distance");
	ClassDB::bind_method(D_METHOD("set_vertical_view_distance", "v"), &VoxelViewer::set_vertical_view_distance);
	ClassDB::bind_method(D_METHOD("get_vertical_view_distance"), &VoxelViewer::get_vertical_view_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertical_view_distance"), "set_vertical_view_distance", "get_vertical_view_distance");
	ClassDB::bind_method(D_METHOD("set_collision_distance", "v"), &VoxelViewer::set_collision_distance);
	ClassDB::bind_method(D_METHOD("get_collision_distance"), &VoxelViewer::get_collision_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_distance"), "set_collision_distance", "get_collision_distance");
	ClassDB::bind_method(D_METHOD("set_enabled", "v"), &VoxelViewer::set_enabled);
	ClassDB::bind_method(D_METHOD("get_enabled"), &VoxelViewer::get_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
}
