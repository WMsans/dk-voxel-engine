#pragma once

// Registers the addon's core classes. A game compiles the addon's sources, register_types.cpp included
// (everything but entry.cpp, which holds the entry symbol the game replaces), and calls this from its
// own module initializer, followed by dk_voxel_register_terrain() if it uses the shipped profiles.
void dk_voxel_register_types();
