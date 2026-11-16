"""Import a Six Guns `.pig` model into Blender (meshes, UVs, game textures, armature + skin weights).

Run from Blender's Scripting tab (open this file, set PIG below, Run Script), or from a terminal:
  /Applications/Blender.app/Contents/MacOS/Blender --python tools/modding/blender_import_pig.py -- \
      <model.pig> [--textures <folder with the model's .tga textures>] [--lod 1] [--render out.png]

- One object per sub-mesh of the chosen LOD (default: the most detailed one), named after its node group.
- Textures: the model names its textures (e.g. TopHat_Df.tga); pass the folder holding the game's
  .tga files (extract them from the OBB) and they are decoded with texture_tool.py (ETC1) and applied.
- Skinned characters get an armature from the model's node tree and vertex groups from the bone
  weights, so they can be posed. Units are metres, Z up, like the game.
- --render writes a quick preview image (used to check the importer headless).
Format reader: pig_format.py (same folder). Exporting back to .pig is not written yet.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bpy  # noqa: E402
import mathutils  # noqa: E402

import pig_format as pf  # noqa: E402

PIG = ""  # set when running from Blender's text editor


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    opts = dict(pig=PIG, textures=None, lod=None, render=None)
    i = 0
    while i < len(a):
        if a[i] == "--textures":
            opts["textures"] = a[i + 1]; i += 2
        elif a[i] == "--lod":
            opts["lod"] = int(a[i + 1]); i += 2
        elif a[i] == "--render":
            opts["render"] = a[i + 1]; i += 2
        else:
            opts["pig"] = a[i]; i += 1
    return opts


def load_texture(folder, name, cache):
    if not folder or name in cache:
        return cache.get(name)
    path = os.path.join(folder, name)
    if not os.path.exists(path):
        cache[name] = None
        return None
    import texture_tool as tt
    import numpy as np
    rgb, alpha, _ = tt.read_texture(path)
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=alpha is not None)
    rgba = np.dstack([rgb, alpha if alpha is not None else np.full((h, w), 255, np.uint8)]).astype(np.float32) / 255
    img.pixels.foreach_set(rgba[::-1].ravel())  # Blender rows go bottom-up
    img.pack()
    cache[name] = img
    return img


def material_for(sm, folder, tcache, mcache):
    key = (sm["material"], tuple(sm["textures"]))
    if key in mcache:
        return mcache[key]
    mat = bpy.data.materials.new(sm["material"] or "pig")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    diffuse = next((t for t in sm["textures"] if t.lower().endswith("_df.tga")), sm["textures"][0] if sm["textures"] else None)
    img = load_texture(folder, diffuse, tcache) if diffuse else None
    if img and bsdf:
        tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
        tex.image = img
        mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    mcache[key] = mat
    return mat


def build_armature(model, name):
    world = pf.node_world(model["nodes"])
    arm = bpy.data.armatures.new(name + "_rig")
    obj = bpy.data.objects.new(name + "_rig", arm)
    bpy.context.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bones = []
    for i, n in enumerate(model["nodes"]):
        b = arm.edit_bones.new(n["name"] or f"node{i}")
        M = world[i]
        head = mathutils.Vector((M[0][3], M[1][3], M[2][3]))
        axis = mathutils.Vector((M[0][2], M[1][2], M[2][2]))  # node's local Z
        b.head = head
        b.tail = head + axis.normalized() * 0.05 if axis.length > 1e-6 else head + mathutils.Vector((0, 0, 0.05))
        bones.append(b)
    for i, n in enumerate(model["nodes"]):
        if n["parent"] >= 0:
            bones[i].parent = bones[n["parent"]]
    names = [b.name for b in bones]
    bpy.ops.object.mode_set(mode="OBJECT")
    return obj, names


def import_pig(path, folder=None, lod=None):
    model = pf.parse(open(path, "rb").read())
    base = os.path.splitext(os.path.basename(path))[0]
    lods = sorted({m["lod"] for m in model["meshes"]})
    lod = lod if lod is not None else lods[0]
    rig, bone_names = (None, None)
    if any(m["skinned"] for m in model["meshes"]):
        rig, bone_names = build_armature(model, base)
    tcache, mcache, made = {}, {}, []
    for mi, me in enumerate(model["meshes"]):
        if me["lod"] != lod:
            continue
        for si, sm in enumerate(me["subs"]):
            pos = pf.positions(model, sm, me["skinned"])
            idx = sm["idx"]
            faces = [tuple(idx[k:k + 3]) for k in range(0, len(idx) - 2, 3)]
            mesh = bpy.data.meshes.new(f"{base}_{mi}_{si}")
            mesh.from_pydata([tuple(p) for p in pos], [], faces)
            if sm["uv"]:
                uvl = mesh.uv_layers.new(name="UVMap")
                uv = sm["uv"][0]
                for li, loop in enumerate(mesh.loops):
                    u, v = uv[loop.vertex_index]
                    uvl.data[li].uv = (u, 1.0 - v)
            mesh.materials.append(material_for(sm, folder, tcache, mcache))
            mesh.validate()
            obj = bpy.data.objects.new(mesh.name, mesh)
            bpy.context.collection.objects.link(obj)
            if me["skinned"] and rig:
                for b in sm["bones"]:
                    vg = obj.vertex_groups.get(bone_names[b["node"]]) or obj.vertex_groups.new(name=bone_names[b["node"]])
                    for vi, _off, w in b["influences"]:
                        vg.add([vi], w, "ADD")
                obj.parent = rig
                mod = obj.modifiers.new("Armature", "ARMATURE")
                mod.object = rig
            made.append(obj)
    print(f"[pig] {path}: LOD {lod} of {lods}, {len(made)} objects, textures "
          f"{sorted(k for k, v in tcache.items() if v)}")
    return made


def render_preview(objs, out):
    scene = bpy.context.scene
    lo = mathutils.Vector((1e9, 1e9, 1e9))
    hi = -lo
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            lo = mathutils.Vector(map(min, lo, w))
            hi = mathutils.Vector(map(max, hi, w))
    centre, size = (lo + hi) / 2, max((hi - lo).length, 0.1)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    cam.location = centre + mathutils.Vector((size * 0.9, -size * 1.6, size * 0.25))
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.rotation_euler = (0.8, 0.2, 0.6)
    scene.collection.objects.link(sun)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x, scene.render.resolution_y = 600, 800
    scene.render.filepath = out
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    o = args()
    if not o["pig"]:
        sys.exit(__doc__)
    if "--background" in sys.argv or o["render"]:
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
    objs = import_pig(o["pig"], o["textures"], o["lod"])
    if o["render"]:
        render_preview(objs, o["render"])
