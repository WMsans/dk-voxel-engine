#!/usr/bin/env python3
"""Builds the demo's material textures from the voxel-everything PBR set.

Every texture the demo shows is a real image file at TEXTURE_ARRAY_SIZE (256) RGBA8, so packing is a straight
copy and the shader's two colours stay white. Tints are baked here, never applied at run time.
Roughness and AO are data maps: copied straight through, no tint and no sRGB encode.

    python3 demo/textures/make_textures.py /path/to/voxel-everything/assets/materials
"""
import os
import sys

import numpy as np
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Development/Godot/voxel-everything/assets/materials")
OUT = os.path.dirname(os.path.abspath(__file__))
SIZE = 256  # must match TEXTURE_ARRAY_SIZE in addons/dk_voxel/src/core/voxel_materials.cpp

# demo material -> (source asset number, albedo tint, albedo gain)
MAP = {
	"grass": ("00", (1.0, 1.0, 1.0), 1.0),
	"soil": ("02", (1.0, 1.0, 1.0), 1.0),
	"rock": ("01", (1.0, 1.0, 1.0), 1.0),
	"sand": ("03", (1.0, 1.0, 1.0), 1.0),
	"gold": ("03", (1.0, 0.78, 0.25), 1.35),  # breakstone reads as gold ore once tinted
	"bedrock": ("01", (0.55, 0.45, 0.60), 0.55),
	"brick": ("03", (0.78, 0.30, 0.22), 1.0),
	"crystal": ("06", (0.55, 0.90, 1.0), 1.15),
}


def srgb_to_linear(c):
	return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
	c = np.clip(c, 0.0, 1.0)
	return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def load(kind, asset):
	img = Image.open(os.path.join(SRC, f"{asset}_{kind}.png")).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
	return np.asarray(img, dtype=np.float32) / 255.0


def save(name, rgb):
	rgba = np.concatenate([rgb, np.ones((SIZE, SIZE, 1), np.float32)], axis=2)
	Image.fromarray((np.clip(rgba, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(OUT, name))


for name, (asset, tint, gain) in MAP.items():
	srgb = load("basecolor", asset)
	linear = srgb_to_linear(srgb) * np.array(tint, np.float32) * gain
	save(f"{name}_albedo.png", linear_to_srgb(linear))
	save(f"{name}_normal.png", load("normal", asset))
	save(f"{name}_roughness.png", load("roughness", asset))
	save(f"{name}_ao.png", load("ambientOcclusion", asset))
	save(f"{name}_height.png", load("height", asset))
	print(f"{name}: {asset} basecolor + normal + roughness + ao + height")
