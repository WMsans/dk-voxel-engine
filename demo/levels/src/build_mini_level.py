"""Builds mini_level.blend, a small Bananza-style stage: a floating island, a rocky peak with a tunnel and
gold veins, a hollow brick tower, a crater ringed by crystals, all as ordered paste/erase/paint parts.

    blender -b --python demo/levels/src/build_mini_level.py

Then edit the .blend by hand and re-export with addons/dk_voxel/tools/blender/dkv_export.py (see the README).
"""
import math
import os

import bmesh
import bpy
from mathutils import Vector, noise

HERE = os.path.dirname(os.path.abspath(__file__))
IDS = {"Grass": 1, "Soil": 2, "Rock": 3, "Sand": 4, "Gold": 5, "Bedrock": 6, "Brick": 7, "Crystal": 8}  # demo/main.gd order
COLORS = {"Grass": (0.2, 0.5, 0.1), "Soil": (0.35, 0.2, 0.1), "Rock": (0.4, 0.4, 0.45), "Sand": (0.75, 0.6, 0.35),
          "Gold": (1.0, 0.7, 0.1), "Bedrock": (0.1, 0.08, 0.15), "Brick": (0.55, 0.25, 0.18), "Crystal": (0.3, 0.5, 0.9)}

bpy.ops.wm.read_factory_settings(use_empty=True)
mats = {}
for name, mid in IDS.items():
    m = bpy.data.materials.new(name)
    m["dk_material"] = mid
    m.diffuse_color = (*COLORS[name], 1.0)
    mats[name] = m


def part(name, shape, at, prio, op="paste", mat="Rock", bumps=0.0, rot=(0, 0, 0), squash_top=1.0):
    bm = bmesh.new()
    shape(bm)
    if squash_top != 1.0:
        for v in bm.verts:
            if v.co.z > 0:
                v.co.z *= squash_top
    if bumps:
        bm.normal_update()
        for v in bm.verts:
            v.co += v.normal * bumps * noise.noise(v.co * 0.18 + Vector((at[0], at[1], 0.0)) * 0.05)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    o.location = at
    o.rotation_euler = rot
    o["dk_op"] = op
    o["dk_priority"] = prio
    o.data.materials.append(mats[mat])
    if op != "paste":
        o.display_type = "WIRE"
    bpy.context.scene.collection.objects.link(o)
    return o


def ellipsoid(rx, ry, rz):
    def make(bm):
        bmesh.ops.create_uvsphere(bm, u_segments=64, v_segments=32, radius=1.0)
        bmesh.ops.scale(bm, vec=(rx, ry, rz), verts=bm.verts)
    return make


def cylinder(r, depth, segs=32, r2=None):
    return lambda bm: bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r,
                                            radius2=r if r2 is None else r2, depth=depth)


def box(sx, sy, sz):
    def make(bm):
        bmesh.ops.create_cube(bm, size=1.0)
        bmesh.ops.scale(bm, vec=(sx, sy, sz), verts=bm.verts)
    return make


def ball(r):
    return lambda bm: bmesh.ops.create_icosphere(bm, subdivisions=3, radius=r)


# Terrain mass, then layers painted onto it (Bananza's VoxelMaterialChangeArea).
part("Island", ellipsoid(30, 30, 9), (0, 0, 4), 0, mat="Rock", bumps=1.2, squash_top=0.35)
part("BedrockCore", ellipsoid(20, 20, 4.5), (0, 0, -1), 1, mat="Bedrock")
part("SoilLayer", cylinder(33, 12), (0, 0, 6), 2, op="paint", mat="Soil")
part("GrassLayer", cylinder(33, 10), (0, 0, 9.5), 3, op="paint", mat="Grass")
part("Beach", ellipsoid(10, 16, 5), (27, 4, 5), 4, op="paint", mat="Sand")

# A rocky peak with gold veins and a tunnel through it.
part("Peak", cylinder(12, 16, 40, r2=2.5), (-11, 8, 13), 10, mat="Rock", bumps=1.0)
for i, p in enumerate([(-6.5, 5, 10), (-14, 3, 12), (-9, 13, 15), (-16, 11, 9)]):
    part(f"GoldVein{i}", ball(1.6), p, 11, mat="Gold")
part("Tunnel", cylinder(2.6, 34), (-11, 8, 9.5), 12, op="erase", rot=(0, math.pi / 2, 0))

# A hollow brick tower with a door.
part("Tower", cylinder(4.5, 12, 24), (14, -11, 11), 20, mat="Brick")
part("TowerHollow", cylinder(3.3, 12), (14, -11, 14), 21, op="erase")
part("TowerDoor", box(2.4, 3, 3.6), (14, -15, 8.6), 22, op="erase")

# A crater ringed by crystals.
part("Crater", ball(5), (8, 13, 8), 30, op="erase")
for i in range(5):
    a = i / 5 * math.tau
    part(f"Crystal{i}", cylinder(0.9, 5, 6, r2=0.05), (8 + 6.2 * math.cos(a), 13 + 6.2 * math.sin(a), 8),
         31, mat="Crystal", rot=(0.35 * math.sin(a), -0.35 * math.cos(a), a))

spawn = bpy.data.objects.new("Spawn", None)
spawn.location = (0, -14, 9)
spawn.empty_display_type = "SINGLE_ARROW"
bpy.context.scene.collection.objects.link(spawn)

bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "mini_level.blend"))
