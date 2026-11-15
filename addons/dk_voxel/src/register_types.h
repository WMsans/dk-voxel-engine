#pragma once

// Registers every addon class. Games compile the class sources but not register_types.cpp,
// and call this from their own module initializer.
void dk_voxel_register_types();