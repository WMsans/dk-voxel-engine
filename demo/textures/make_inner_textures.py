#!/usr/bin/env python3
"""Builds the demo's inner ("dug") material textures from the terrain_textures_vol2 pack.

Same contract as make_textures.py: 256 px RGBA8 files, roughness and AO copied as data. The pack is
OpenGL-normal (Y+), which is what Godot expects.

    python3 demo/textures/make_inner_textures.py /path/to/terrain_textures_vol2
"""
import os
import sys

from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Development/Unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2")
OUT = os.path.dirname(os.path.abspath(__file__))
SIZE = 256  # must match TEXTURE_ARRAY_SIZE in addons/dk_voxel/src/core/voxel_materials.cpp

# demo material -> pack folder
MAP = {
	"turf": "ground_foliage_01",  # grass torn open: soil with roots
	"dug_soil": "ground_crack_01",  # soil broken into clods
	"broken_rock": "ground_stone_01",  # rock shattered into plates
	"rubble": "breakstone",  # brick crushed to grit
	"cracked_crystal": "ice_crack",  # crystal split along facets
}
KINDS = {"albedo": "basecolor", "normal": "normal", "roughness": "roughness", "ao": "ambientOcclusion"}

for name, folder in MAP.items():
	for kind, src_kind in KINDS.items():
		img = Image.open(os.path.join(SRC, folder, f"T_{folder}_{src_kind}.tga")).convert("RGB").convert("RGBA")
		img.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, f"{name}_{kind}.png"))
	print(f"{name}: {folder}")
