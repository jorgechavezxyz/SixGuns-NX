"""Import Six Guns `.anim` files as Blender actions on a rig made by blender_import_pig.py.

From a terminal (imports the model too, then the animations; the first one is the active action):
  /Applications/Blender.app/Contents/MacOS/Blender --python tools/modding/blender_import_anim.py -- \
      characters/outlawtophat.pig --textures tex/ anims/human_duel_idle.anim anims/human_duel_drawb.anim \
      [--play human_duel_drawb] [--render out_prefix --frames 0,0.5,1] [--save out.blend]
From Blender's Scripting tab: run blender_import_pig.py first, then set PIG / ANIMS below and run this file
(or call import_anim(rig, pig_path, anim_path) from your own script).

- One action per file (named after the file, with a fake user), keyframes at frame = ms * fps / 1000 on the
  scene (the scene fps is set to the clip's fps), so Blender plays the game's timing. Switch clips with the
  Action selector of the rig (Dope Sheet > Action Editor) or `rig.animation_data.action`.
- Bones are matched to animation nodes by name. Animation nodes without a bone in the model (Master,
  Bip01_Footsteps, Toe0: root-motion / helper nodes) are skipped.
- Rotation: bones are created by blender_import_pig.py with head = node origin and an arbitrary roll, so
  bone.matrix_local = nodeWorld_rest * C with a constant rotation C per bone. For a node whose local transform
  goes from L0 (rest) to L (animation), the pose-bone basis is  C^-1 * L0^-1 * L * C.  The engine's quaternion
  convention is the transpose of the usual one (pig_format.quat_mat), which is used to build L0 and L.
- Hold keys (top bit of the key time) become constant interpolation on that key.
Format reader: anim_format.py (same folder).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bpy  # noqa: E402
import mathutils  # noqa: E402

import anim_format as af  # noqa: E402
import pig_format as pf  # noqa: E402

PIG = ""
ANIMS = []  # list of .anim paths when run from the Scripting tab


def _mat4(R3, pos, scale=(1, 1, 1)):
    m = mathutils.Matrix.Identity(4)
    for r in range(3):
        for c in range(3):
            m[r][c] = R3[r][c] * scale[c]
        m[r][3] = pos[r]
    return m


def _local(pos, quat, scale):
    return _mat4(pf.quat_mat(quat), pos, scale)


def rig_info(rig, model):
    """Per node name: (bone name, rest local matrix L0, constant C). Rest world matrices from the model."""
    world = pf.node_world(model["nodes"])
    info = {}
    for i, n in enumerate(model["nodes"]):
        name = n["name"] or f"node{i}"
        bone = rig.data.bones.get(name)
        if bone is None:
            continue
        W0 = mathutils.Matrix([list(r) for r in world[i]] + [[0, 0, 0, 1]])
        W0r = W0.to_3x3().normalized()  # strip scale: bones are rigid
        C = W0r.inverted() @ bone.matrix_local.to_3x3()
        L0 = _local(n["pos"], n["rot"], n["scale"])
        info[name] = (bone, L0, C.to_4x4())
    return info


def _basis(L0, L, C):
    B = C.inverted() @ L0.inverted() @ L @ C
    return B.to_translation(), B.to_quaternion(), B.to_scale()


def _channelbag(action, slot):
    from bpy_extras import anim_utils
    return anim_utils.action_ensure_channelbag_for_slot(action, slot)


def import_anim(rig, model, anim_path, name=None, make_active=True):
    """Create one action from an .anim file on `rig`; returns the action."""
    data = open(anim_path, "rb").read()
    anim = af.parse(data)
    name = name or os.path.splitext(os.path.basename(anim_path))[0]
    info = rig_info(rig, model)
    nodes_rest = {(n["name"] or f"node{i}"): n for i, n in enumerate(model["nodes"])}
    fps = anim["fps"] or 20
    bpy.context.scene.render.fps = fps
    bpy.context.scene.render.fps_base = 1.0

    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    if rig.animation_data is None:
        rig.animation_data_create()
    rig.animation_data.action = action
    for pb in rig.pose.bones:
        pb.rotation_mode = "QUATERNION"

    last = 0.0
    for ni, nd in enumerate(anim["nodes"]):
        if nd["name"] not in info:
            continue
        bone, L0, C = info[nd["name"]]
        rest = nodes_rest[nd["name"]]
        pb = rig.pose.bones[nd["name"]]
        times = set()
        hold = {}
        for k in ("pos", "rot", "scale"):
            tr = nd[k]
            if tr[0] == "keys":
                for t in tr[1]:
                    ms = af.key_ms(t)
                    times.add(ms)
                    if af.key_hold(t):
                        hold[ms] = True
        if not times:
            times = {0}  # constant tracks: one key
        prev = None
        for ms in sorted(times):
            p = af.sample_track(nd["pos"], ms)
            q = af.sample_track(nd["rot"], ms, True)
            s = af.sample_track(nd["scale"], ms)
            loc, rot, scl = _basis(L0, _local(p, q, s), C)
            if prev is not None and prev.dot(rot) < 0:
                rot = -rot
            prev = rot
            f = ms * fps / 1000.0
            last = max(last, f)
            pb.location, pb.rotation_quaternion, pb.scale = loc, rot, scl
            pb.keyframe_insert("location", frame=f, group=nd["name"])
            pb.keyframe_insert("rotation_quaternion", frame=f, group=nd["name"])
            pb.keyframe_insert("scale", frame=f, group=nd["name"])
    # hold keys -> constant interpolation
    if hold_any(anim):
        slot = rig.animation_data.action_slot
        cb = _channelbag(action, slot)
        holds = {}
        for nd in anim["nodes"]:
            ms_hold = {af.key_ms(t) * fps / 1000.0 for k in ("pos", "rot", "scale") if nd[k][0] == "keys"
                       for t in nd[k][1] if af.key_hold(t)}
            if ms_hold:
                holds[nd["name"]] = ms_hold
        for fc in cb.fcurves:
            bn = fc.data_path.split('"')[1] if '"' in fc.data_path else None
            for kp in fc.keyframe_points:
                if bn in holds and any(abs(kp.co[0] - h) < 1e-4 for h in holds[bn]):
                    kp.interpolation = "CONSTANT"
    action["sixguns_fps"] = fps
    action["sixguns_duration_ms"] = anim["duration_ms"]
    action.frame_range = (0, max(last, 1))
    if not make_active:
        rig.animation_data.action = None
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 0, int(round(last))
    print(f"[anim] {name}: {anim['duration_ms']} ms @ {fps} fps, {sum(1 for n in anim['nodes'] if n['name'] in info)}"
          f"/{len(anim['nodes'])} nodes mapped, action frames 0..{last:.1f}")
    return action


def hold_any(anim):
    return any(af.key_hold(t) for n in anim["nodes"] for k in ("pos", "rot", "scale")
               if n[k][0] == "keys" for t in n[k][1])


def find_rig():
    for o in bpy.data.objects:
        if o.type == "ARMATURE":
            return o
    return None


def render_frames(objs, rig, prefix, fracs, size=(600, 800)):
    scene = bpy.context.scene
    scene.frame_set(0)
    bpy.context.view_layer.update()
    lo = mathutils.Vector((1e9, 1e9, 1e9))
    hi = -lo
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            lo = mathutils.Vector(map(min, lo, w))
            hi = mathutils.Vector(map(max, hi, w))
    centre, ext = (lo + hi) / 2, (hi - lo)
    d = max(ext.length, 0.5) * 1.6
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    cam.location = centre + mathutils.Vector((d * 0.35, -d, d * 0.12))
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.rotation_euler = (0.8, 0.2, 0.6)
    scene.collection.objects.link(sun)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x, scene.render.resolution_y = size
    out = []
    f0, f1 = scene.frame_start, scene.frame_end
    for i, fr in enumerate(fracs):
        scene.frame_set(int(round(f0 + (f1 - f0) * fr)))
        path = os.path.abspath(f"{prefix}_{i}.png")
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        out.append(path)
    return out


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(pig=PIG, textures=None, lod=None, anims=list(ANIMS), play=None, render=None,
             frames=[0.0, 0.5, 1.0], save=None)
    i = 0
    while i < len(a):
        if a[i] == "--textures": o["textures"] = a[i + 1]; i += 2
        elif a[i] == "--lod": o["lod"] = int(a[i + 1]); i += 2
        elif a[i] == "--play": o["play"] = a[i + 1]; i += 2
        elif a[i] == "--render": o["render"] = a[i + 1]; i += 2
        elif a[i] == "--frames": o["frames"] = [float(x) for x in a[i + 1].split(",")]; i += 2
        elif a[i] == "--save": o["save"] = a[i + 1]; i += 2
        elif a[i].lower().endswith(".anim"): o["anims"].append(a[i]); i += 1
        else: o["pig"] = a[i]; i += 1
    return o


if __name__ == "__main__":
    o = args()
    if not o["pig"] or not o["anims"]:
        sys.exit(__doc__)
    import blender_import_pig as bip
    objs = []
    rig = None
    if o["pig"].lower().endswith(".pig"):
        if "--background" in sys.argv:
            for ob in list(bpy.data.objects):
                bpy.data.objects.remove(ob)
        objs = bip.import_pig(o["pig"], o["textures"], o["lod"])
        rig = find_rig()
    else:
        rig = find_rig()
    if rig is None:
        sys.exit("no armature: the model has no skinned meshes")
    model = pf.parse(open(o["pig"], "rb").read())
    actions = [import_anim(rig, model, p, make_active=False) for p in o["anims"]]
    play = next((a for a in actions if a.name == o["play"]), actions[-1]) if o["play"] or actions else None
    rig.animation_data.action = play
    scn = bpy.context.scene
    scn.frame_start, scn.frame_end = 0, int(round(play.frame_range[1]))
    if o["render"]:
        print(render_frames(objs or [x for x in bpy.data.objects if x.type == "MESH"], rig, o["render"], o["frames"]))
    if o["save"]:
        bpy.ops.wm.save_as_mainfile(filepath=o["save"])
