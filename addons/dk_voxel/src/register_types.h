#pragma once

// Registers every addon class. A game compiles the addon's sources, register_types.cpp included
// (everything but entry.cpp, which holds the entry symbol the game replaces), and calls this from its
// own module initializer.
void dk_voxel_register_types();