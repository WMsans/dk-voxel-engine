// The addon's GDExtension entry point. Only this translation unit defines dk_voxel_library_init;
// games that compile the addon sources into their own library provide their own entry symbol.
#include "register_types.h"
#include "voxel_resources.h"
#include "voxel_world.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void dk_voxel_register_types() {
	GDREGISTER_ABSTRACT_CLASS(VoxelGenerator);
	GDREGISTER_CLASS(DemoTerrainGenerator);
	GDREGISTER_CLASS(VoxelMaterialDef);
	GDREGISTER_CLASS(VoxelMaterialSet);
	GDREGISTER_CLASS(VoxelWorld);
}

static void dk_voxel_init(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE) dk_voxel_register_types();
}
static void dk_voxel_uninit(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT dk_voxel_library_init(GDExtensionInterfaceGetProcAddress get_proc, GDExtensionClassLibraryPtr library, GDExtensionInitialization *init) {
	GDExtensionBinding::InitObject obj(get_proc, library, init);
	obj.register_initializer(dk_voxel_init);
	obj.register_terminator(dk_voxel_uninit);
	obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return obj.init();
}