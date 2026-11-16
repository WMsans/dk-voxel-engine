// Registers every addon class. The GDExtension entry symbol the addon library uses lives in entry.cpp;
// games that compile these sources into their own library keep this file and provide their own.
#include "register_types.h"
#include "voxel_materials.h"
#include "voxel_resources.h"
#include "voxel_world.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void dk_voxel_register_types() {
	GDREGISTER_ABSTRACT_CLASS(VoxelGenerator);
	GDREGISTER_CLASS(DemoTerrainGenerator);
	GDREGISTER_CLASS(LevelTerrainGenerator);
	GDREGISTER_CLASS(VoxelMaterialDef);
	GDREGISTER_CLASS(VoxelMaterialSet);
	GDREGISTER_CLASS(VoxelViewer);
	GDREGISTER_CLASS(VoxelWorld);
}
