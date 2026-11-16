"""Export edited Six Guns model geometry from Blender back to a `.pig` (counterpart of blender_import_pig.py).

  Blender --background <scene.blend> --python tools/modding/blender_export_pig.py -- <original.pig> <out.pig>
  (or run inside Blender after editing the objects the importer made)

How it works: the original .pig is the template (node tree, materials, textures, LOD groups, flags stay).
Every object named like the importer names them (<model>_<mesh>_<sub>) replaces that sub-mesh's geometry:
- vertices are split where a vertex has different UVs on different faces (the game stores one UV per vertex);
- faces are triangulated; normals and tangents are recomputed by Blender;
- static meshes: positions/normals (and tangent arrays when the original had them) go into the blob;
- skinned meshes: every vertex becomes one record per bone it is weighted to (vertex groups named after the
  model's nodes, weights normalised, at most 4): offset = inverse(bone world) * position, unscaled (the
  engine multiplies by the weight at load), plus the normal and tangent rotated into bone space.
Objects you don't edit are written back unchanged. Edit in object space with the object and rig left at the
origin (don't move/rotate the objects themselves; edit the mesh).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bpy  # noqa: E402
import mathutils  # noqa: E402

import pig_format as pf  # noqa: E402


def mat4(m34):
    return mathutils.Matrix([m34[0], m34[1], m34[2], [0, 0, 0, 1]])


def sub_from_object(obj, model, me, sm, world_inv, node_index):
    mesh = obj.data
    mesh.calc_loop_triangles()
    uvl = mesh.uv_layers.active
    has_tan = uvl is not None
    if has_tan:
        mesh.calc_tangents(uvmap=uvl.name)
    verts, key_to_new, idx = [], {}, []
    for tri in mesh.loop_triangles:
        for li in tri.loops:
            loop = mesh.loops[li]
            v = loop.vertex_index
            uv = tuple(round(c, 6) for c in uvl.data[li].uv) if uvl else (0.0, 0.0)
            key = (v, uv)
            if key not in key_to_new:
                key_to_new[key] = len(verts)
                tan = tuple(loop.tangent) if has_tan else (1.0, 0.0, 0.0)
                verts.append(dict(v=v, uv=(uv[0], 1.0 - uv[1]), nrm=tuple(loop.normal), tan=tan))
            idx.append(key_to_new[key])
    if len(verts) > 65535:
        raise SystemExit(f"{obj.name}: {len(verts)} vertices (max 65535)")
    pos = [tuple(mesh.vertices[x["v"]].co) for x in verts]
    new = dict(sm)
    new["idx"] = idx
    new["vcount"] = len(verts)
    new["uv"] = [[x["uv"] for x in verts]] + [None] * 0
    if len(sm["uv"]) > 1:  # extra UV sets: reuse set 0 (the game uses set 0 for the diffuse map)
        new["uv"] += [[x["uv"] for x in verts] for _ in sm["uv"][1:]]
    if sm.get("colour"):
        new["colour"] = [sm["colour"][0]] * len(verts) if len(sm["colour"]) != len(verts) else sm["colour"]
    lo = [min(p[i] for p in pos) for i in range(3)]
    hi = [max(p[i] for p in pos) for i in range(3)]
    new["centre"] = [(lo[i] + hi[i]) / 2 for i in range(3)]
    if not me["skinned"]:
        new["pos"] = pos
        new["pos4"] = None
        new["nrm"] = [x["nrm"] for x in verts]
        new["nrm4"] = None
        if sm["ext"]:
            # tangent-like arrays: normal / tangent / binormal with w = 1
            ext = []
            for k in range(len(sm["ext"])):
                arr = []
                for x in verts:
                    n, t = mathutils.Vector(x["nrm"]), mathutils.Vector(x["tan"])
                    vec = [n, t, n.cross(t)][k % 3]
                    arr.append((vec.x, vec.y, vec.z, 1.0))
                ext.append(arr)
            new["ext"] = ext
        return new, (lo, hi)
    # skinned: bone records from vertex groups
    groups = {g.index: g.name for g in obj.vertex_groups}
    per_bone = {}
    for ni, x in enumerate(verts):
        vg = [(groups[g.group], g.weight) for g in mesh.vertices[x["v"]].groups if g.weight > 1e-4 and groups.get(g.group) in node_index]
        vg = sorted(vg, key=lambda t: -t[1])[:4]
        tot = sum(w for _, w in vg)
        if not vg or tot <= 0:
            raise SystemExit(f"{obj.name}: vertex {x['v']} has no weight on any bone of the model")
        p = mathutils.Vector(pos[ni])
        n, t = mathutils.Vector(x["nrm"]), mathutils.Vector(x["tan"])
        for name, wgt in vg:
            bi = node_index[name]
            inv = world_inv[bi]
            off = inv @ p
            rinv = inv.to_3x3()
            noff, toff = rinv @ n, rinv @ t
            per_bone.setdefault(bi, []).append((ni, tuple(off), tuple(noff), tuple(toff), wgt / tot))
    new["bones"] = [dict(node=bi, records=recs, influences=[(r[0], r[1], r[4]) for r in recs])
                    for bi, recs in sorted(per_bone.items())]
    new["remap"], new["bind"] = [], {}
    return new, (lo, hi)


def export(original, out):
    model = pf.parse(open(original, "rb").read())
    base = os.path.splitext(os.path.basename(original))[0]
    world = pf.node_world(model["nodes"])
    world_inv = [mat4(w).inverted() for w in world]
    node_index = {n["name"] or f"node{i}": i for i, n in enumerate(model["nodes"])}
    changed = 0
    for mi, me in enumerate(model["meshes"]):
        bb = None
        for si, sm in enumerate(me["subs"]):
            obj = bpy.data.objects.get(f"{base}_{mi}_{si}")
            if not obj or obj.type != "MESH":
                continue
            new, (lo, hi) = sub_from_object(obj, model, me, sm, world_inv, node_index)
            me["subs"][si] = new
            bb = (lo, hi) if bb is None else ([min(a, b) for a, b in zip(bb[0], lo)], [max(a, b) for a, b in zip(bb[1], hi)])
            changed += 1
        if bb:
            me["bbox"] = list(bb[0]) + list(bb[1])
    data = pf.write(model)
    pf.parse(data)  # sanity: the result reads back
    open(out, "wb").write(data)
    print(f"[pig] wrote {out}: {changed} sub-meshes from Blender objects, {len(data)} bytes")


if __name__ == "__main__":
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(a) < 2:
        sys.exit(__doc__)
    export(a[0], a[1])
