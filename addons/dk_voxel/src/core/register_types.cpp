// Registers the addon's core classes. Terrain profiles register separately (terrain/terrain_resources.h), so
// a game picks which generators it ships. The addon's own entry symbol lives in ../entry.cpp; games that
// compile these sources into their own library keep this file and provide their own.
#include "register_types.h"
#include "voxel_materials.h"
#include "voxel_resources.h"
#include "voxel_world.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void dk_voxel_register_types() {
	GDREGISTER_ABSTRACT_CLASS(VoxelGenerator);
	GDREGISTER_CLASS(VoxelMaterialDef);
	GDREGISTER_CLASS(VoxelMaterialSet);
	GDREGISTER_CLASS(VoxelViewer);
	GDREGISTER_CLASS(VoxelWorld);
}
