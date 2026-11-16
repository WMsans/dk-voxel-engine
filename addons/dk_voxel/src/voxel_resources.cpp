#include "voxel_resources.h"

#include "demo_generator.h"

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

void VoxelMaterialDef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_name", "v"), &VoxelMaterialDef::set_name);
	ClassDB::bind_method(D_METHOD("get_name"), &VoxelMaterialDef::get_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ClassDB::bind_method(D_METHOD("set_color_a", "v"), &VoxelMaterialDef::set_color_a);
	ClassDB::bind_method(D_METHOD("get_color_a"), &VoxelMaterialDef::get_color_a);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_a"), "set_color_a", "get_color_a");
	ClassDB::bind_method(D_METHOD("set_color_b", "v"), &VoxelMaterialDef::set_color_b);
	ClassDB::bind_method(D_METHOD("get_color_b"), &VoxelMaterialDef::get_color_b);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_b"), "set_color_b", "get_color_b");
	ClassDB::bind_method(D_METHOD("set_pattern_scale", "v"), &VoxelMaterialDef::set_pattern_scale);
	ClassDB::bind_method(D_METHOD("get_pattern_scale"), &VoxelMaterialDef::get_pattern_scale);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pattern_scale"), "set_pattern_scale", "get_pattern_scale");
	ClassDB::bind_method(D_METHOD("set_roughness", "v"), &VoxelMaterialDef::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &VoxelMaterialDef::get_roughness);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness"), "set_roughness", "get_roughness");
	ClassDB::bind_method(D_METHOD("set_metallic", "v"), &VoxelMaterialDef::set_metallic);
	ClassDB::bind_method(D_METHOD("get_metallic"), &VoxelMaterialDef::get_metallic);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic"), "set_metallic", "get_metallic");
	ClassDB::bind_method(D_METHOD("set_emission", "v"), &VoxelMaterialDef::set_emission);
	ClassDB::bind_method(D_METHOD("get_emission"), &VoxelMaterialDef::get_emission);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission"), "set_emission", "get_emission");
	ClassDB::bind_method(D_METHOD("set_hardness", "v"), &VoxelMaterialDef::set_hardness);
	ClassDB::bind_method(D_METHOD("get_hardness"), &VoxelMaterialDef::get_hardness);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hardness"), "set_hardness", "get_hardness");
}

void VoxelMaterialSet::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_materials", "v"), &VoxelMaterialSet::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &VoxelMaterialSet::get_materials);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_TYPE_STRING, "24/17:VoxelMaterialDef"), "set_materials", "get_materials");
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
