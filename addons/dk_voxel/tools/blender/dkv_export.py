"""Export a Blender scene as a dk_voxel level (.dkvl) for LevelTerrainGenerator.

    blender -b level.blend --python dkv_export.py -- out.dkvl [--cell 0.5]

Bananza-style authoring: every mesh object is one part, voxelized offline into a signed-distance grid,
and the parts apply in order. Object custom properties (all optional):
    dk_op        "paste" (default) | "erase" | "paint"   paint recolours solid voxels inside the mesh
    dk_material  material id; else the first material slot's dk_material; else 1
    dk_priority  parts apply lowest first (default 0), ties by object name
    dk_skip      true leaves the object out
An object named "Spawn" (any type) marks where the player starts. Meshes must be closed (watertight).
Axes become Godot's (y up), and the level is shifted so it starts at the origin.
"""
import struct
import sys

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

SD_SCALE = 16.0  # must match LevelTerrain::SD_SCALE
PAD = 3  # cells of air around every part's grid
MODES = {"paste": 0, "erase": 1, "paint": 2}


def to_godot(v):
    return Vector((v.x, v.z, -v.y))


def material_of(obj):
    if "dk_material" in obj:
        return int(obj["dk_material"])
    for slot in obj.material_slots:
        if slot.material is not None and "dk_material" in slot.material:
            return int(slot.material["dk_material"])
    return 1


def voxelize(obj, dg, cell, need_distance):
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    verts = [to_godot(obj.matrix_world @ v.co) for v in me.vertices]
    polys = [tuple(p.vertices) for p in me.polygons]
    ev.to_mesh_clear()
    if not polys:
        return None
    bvh = BVHTree.FromPolygons(verts, polys)
    lo = np.floor(np.min(verts, axis=0) / cell).astype(int) - PAD
    hi = np.ceil(np.max(verts, axis=0) / cell).astype(int) + PAD
    n = hi - lo + 1
    origin = lo * cell
    band = 127.0 / SD_SCALE * cell
    xs = origin[0] + np.arange(n[0]) * cell
    sd = np.empty((n[2], n[1], n[0]), dtype=np.float32)
    ray = Vector((1, 0, 0))
    for k in range(n[2]):
        z = origin[2] + k * cell
        for j in range(n[1]):
            y = origin[1] + j * cell
            # Inside = odd number of crossings to the left. The ray is nudged off the grid lines so it does
            # not graze mesh edges, which would count one crossing twice.
            o = Vector((xs[0] - cell, y + 1.3e-4 * cell, z + 0.7e-4 * cell))
            hits = []
            while True:
                loc, _, _, _ = bvh.ray_cast(o, ray)
                if loc is None:
                    break
                hits.append(loc.x)
                o = Vector((loc.x + 1e-5, o.y, o.z))
            inside = np.searchsorted(np.array(hits), xs) % 2 == 1
            row = np.full(n[0], band, dtype=np.float32)
            if need_distance:
                for i in range(n[0]):
                    near = bvh.find_nearest(Vector((xs[i], y, z)), band)
                    if near[0] is not None:
                        row[i] = near[3]
            sd[k, j] = np.where(inside, -row, row)
    q = np.clip(np.rint(sd / cell * SD_SCALE), -127, 127).astype(np.int8)
    return origin, n, q


def export(path, cell=0.5):
    dg = bpy.context.evaluated_depsgraph_get()
    objs = [o for o in bpy.context.scene.objects if o.type == "MESH" and not o.get("dk_skip", False)]
    objs.sort(key=lambda o: (int(o.get("dk_priority", 0)), o.name))
    parts = []
    for o in objs:
        mode = MODES[str(o.get("dk_op", "paste")).lower()]
        v = voxelize(o, dg, cell, need_distance=mode != MODES["paint"])
        if v is None:
            print(f"dkv_export: {o.name}: no faces, skipped")
            continue
        parts.append((mode, material_of(o), *v))
        print(f"dkv_export: {o.name}: {['paste', 'erase', 'paint'][mode]} material {material_of(o)} grid {tuple(int(x) for x in v[1])}")
    if not parts:
        raise SystemExit("dkv_export: no mesh objects to export")
    shift = np.min([p[2] for p in parts], axis=0)
    size = np.max([p[2] + (p[3] - 1) * cell for p in parts], axis=0) - shift
    sp = bpy.context.scene.objects.get("Spawn")
    spawn = (np.array(to_godot(sp.matrix_world.translation)) if sp else np.array([0.0, 0.0, 0.0])) - shift
    with open(path, "wb") as f:
        f.write(b"DKVL" + struct.pack("<I3f3fI", 1, *size, *spawn, len(parts)))
        for mode, mat, origin, n, q in parts:
            f.write(struct.pack("<BBH3ff3I", mode, mat, 0, *(origin - shift), cell, *n))
            f.write(q.tobytes())
    print(f"dkv_export: wrote {path}: {len(parts)} parts, size {size.round(2).tolist()}, spawn {spawn.round(2).tolist()}")


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        raise SystemExit("usage: blender -b level.blend --python dkv_export.py -- out.dkvl [--cell 0.5]")
    export(args[0], float(args[args.index("--cell") + 1]) if "--cell" in args else 0.5)
