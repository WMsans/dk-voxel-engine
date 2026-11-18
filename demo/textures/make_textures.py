#!/usr/bin/env python3
"""Builds the demo's material textures from the terrain_textures_vol2 pack.

Every texture the demo shows is a real image file at TEXTURE_ARRAY_SIZE (1024) RGBA8, so packing is a straight
copy and the shader's two colours stay white. Tints are baked here, never applied at run time.
Normal, roughness, AO and height are data maps: copied straight through, no tint and no sRGB encode.
The slop pack is imported by addons/dk_voxel/tools/organize_material.sh, which writes this same layout.

    python3 demo/textures/make_textures.py /path/to/terrain_textures_vol2
"""
import os
import sys

import numpy as np
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Development/Unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2")
OUT = os.path.dirname(os.path.abspath(__file__))
SIZE = 1024  # must match TEXTURE_ARRAY_SIZE in addons/dk_voxel/src/core/voxel_materials.cpp

# demo material -> (pack folder, albedo tint, albedo gain); white and 1.0 show the texture as shipped
MAP = {
	"grass": ("grass_01", (1.0, 1.0, 1.0), 1.0),
	"soil": ("mud_01", (1.0, 1.0, 1.0), 1.0),  # "Default" shows these same files
	"rock": ("rock", (1.0, 1.0, 1.0), 1.0),
	"sand": ("sand_01", (1.0, 1.0, 1.0), 1.0),
	"gold": ("stones_02", (1.0, 0.78, 0.25), 1.35),  # stones read as gold ore once tinted
	"bedrock": ("cliff_stone_02", (1.0, 1.0, 1.0), 1.0),
	"crystal": ("ice", (1.0, 1.0, 1.0), 1.0),
	"turf": ("ground_foliage_01", (1.0, 1.0, 1.0), 1.0),  # grass torn open: soil with roots
	"dug_soil": ("ground_crack_01", (1.0, 1.0, 1.0), 1.0),  # soil broken into clods
	"broken_rock": ("ground_stone_01", (1.0, 1.0, 1.0), 1.0),  # rock shattered into plates
	"rubble": ("breakstone", (1.0, 1.0, 1.0), 1.0),  # brick crushed to grit
	"cracked_crystal": ("ice_crack", (1.0, 1.0, 1.0), 1.0),  # crystal split along facets
}
DATA = {"normal": "normal", "roughness": "roughness", "ao": "ambientOcclusion", "height": "height"}


def srgb_to_linear(c):
	return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
	c = np.clip(c, 0.0, 1.0)
	return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def load(folder, kind):
	img = Image.open(os.path.join(SRC, folder, f"T_{folder}_{kind}.tga")).convert("RGB")
	return img.resize((SIZE, SIZE), Image.LANCZOS)


def save(name, kind, img):
	out_dir = os.path.join(OUT, name)
	os.makedirs(out_dir, exist_ok=True)
	img.convert("RGBA").save(os.path.join(out_dir, f"{name}_{kind}.png"))


for name, (folder, tint, gain) in MAP.items():
	albedo = load(folder, "basecolor")
	if tint != (1.0, 1.0, 1.0) or gain != 1.0:  # tint in linear space, then re-encode for the shader
		linear = srgb_to_linear(np.asarray(albedo, np.float32) / 255.0) * np.array(tint, np.float32) * gain
		albedo = Image.fromarray((linear_to_srgb(linear) * 255.0 + 0.5).astype(np.uint8), "RGB")
	save(name, "albedo", albedo)
	for kind, src_kind in DATA.items():
		save(name, kind, load(folder, src_kind))
	print(f"{name}: {folder}")
