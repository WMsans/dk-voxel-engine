// The addon's GDExtension entry point. Only this translation unit defines dk_voxel_library_init; games
// that compile the addon sources into their own library (see ../SConscript) provide their own entry symbol
// and leave this file out.
#include "core/register_types.h"
#include "terrain/terrain_resources.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void dk_voxel_init(ModuleInitializationLevel level) {
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
	dk_voxel_register_types();
	dk_voxel_register_terrain();
}
static void dk_voxel_uninit(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT dk_voxel_library_init(GDExtensionInterfaceGetProcAddress get_proc, GDExtensionClassLibraryPtr library, GDExtensionInitialization *init) {
	GDExtensionBinding::InitObject obj(get_proc, library, init);
	obj.register_initializer(dk_voxel_init);
	obj.register_terminator(dk_voxel_uninit);
	obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return obj.init();
}
