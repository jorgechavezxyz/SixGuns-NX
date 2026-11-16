#!/usr/bin/env python3
"""Stadium multiplayer map (original design for the port: a football field with a running track, high-school
bleachers and a stadium wall as the border), packaged as a custom map with its own lobby page.

  PY=/Applications/Blender.app/Contents/Resources/5.3/python/bin/python3.13    # needs numpy (texture_tool)
  $PY -I build_stadium.py <main.obb> <outdir>

Writes <outdir>/mod/ (OBB layout) and <outdir>/30_stadium.zip (stored zip for switch/sixguns/mods/):
  custom_map.txt                              name, base map, replacement level (port: source/custom_maps.c)
  custommaps/stadium/stadium__a12_tdm_forest_minimap.tga   the minimap during stadium matches
  custommaps/stadium/stadium.bclara           the Canyon level with the forest geometry removed, team spawns
                                              and flags in the end zones, model/collision names below
  models/stadium.pig                          all stadium geometry (one model, one sub-mesh per surface)
  collisions/stadium.coll                     ground, bleachers, goal posts, stadium wall
  textures/stadium_*.tga                      generated textures (ETC1 PVR)
A custom map is an overlay on a base map (here Canyon, index 2): the lobby gets an extra page, the room's map
index is 5 + n, every engine check that a level is a multiplayer map sees Canyon, and the port opens the
replacement level instead of Canyon's only while the room's map is the custom one. Canyon itself is unchanged.
Units: metres, Z up; the stadium is centred where the forest map was (x -715, y 8), so the level's sky,
cameras and limits stay valid.
"""
import copy
import io
import json
import math
import os
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD_TOOLS = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, MOD_TOOLS)
import numpy as np  # noqa: E402
import pig_format as pf  # noqa: E402
import texture_tool as tt  # noqa: E402

CX, CY, Z0 = -715.0, 8.0, 0.0

# ------------------------------------------------------------------ textures (generated, original)


def save_tex(path, rgb):
    png = path[:-4] + ".png"
    tt.write_png(png, rgb.astype(np.uint8))
    tt.build(png, path)
    os.remove(png)


def noise(h, w, amp, seed):
    rng = np.random.default_rng(seed)
    return rng.normal(0, amp, (h, w, 1))


def field_texture():
    # 1024 x 512 covers the whole 120 x 53.3 yd field (end zones included): u along the length, v across
    W, H = 1024, 512
    img = np.zeros((H, W, 3), np.float32)
    x = np.arange(W)[None, :] / W * 120.0  # yards
    stripe = ((x // 5).astype(int) % 2)
    img[:] = np.where(stripe[..., None] == 0, [52, 120, 44], [60, 134, 50])
    img[:, :int(W * 10 / 120)] = [150, 30, 30]          # red end zone
    img[:, int(W * 110 / 120):] = [30, 50, 150]         # blue end zone
    line = lambda yd: slice(max(0, int(W * yd / 120) - 2), int(W * yd / 120) + 2)
    for yd in range(10, 111, 5):
        img[:, line(yd)] = 235
    for yd in range(11, 110):                            # hash marks every yard
        xs = line(yd)
        for vh in (0.33, 0.67):
            img[int(H * vh) - 6:int(H * vh) + 6, xs] = 235
    img[:4, :] = 235; img[-4:, :] = 235; img[:, :4] = 235; img[:, -4:] = 235  # boundary
    img += noise(H, W, 6, 1)
    return np.clip(img, 0, 255)


def track_texture():
    # 256 x 256: v across the 8 lanes, u along the track (tiles)
    img = np.zeros((256, 256, 3), np.float32) + [168, 64, 46]
    img += noise(256, 256, 10, 2)
    for k in range(9):
        y = int(k * 255 / 8)
        img[max(0, y - 1):y + 2, :] = 240
    return np.clip(img, 0, 255)


def flat_texture(rgb, amp, seed, size=128):
    img = np.zeros((size, size, 3), np.float32) + rgb
    img += noise(size, size, amp, seed)
    return np.clip(img, 0, 255)


def bleacher_texture():
    img = np.zeros((128, 128, 3), np.float32) + [182, 186, 192]   # aluminium planks
    for k in range(0, 128, 32):
        img[k:k + 3, :] = [90, 92, 98]
    img += noise(128, 128, 5, 4)
    return np.clip(img, 0, 255)


def wall_texture():
    img = np.zeros((128, 128, 3), np.float32) + [150, 146, 138]   # painted block wall
    img[:24, :] = [36, 70, 140]                                   # blue band at the top
    for k in range(0, 128, 16):
        img[k:k + 1, :] -= 30
    for k in range(0, 128, 32):
        img[:, k:k + 1] -= 25
    img += noise(128, 128, 5, 5)
    return np.clip(img, 0, 255)


TEXTURES = {
    "stadium_field_df.tga": field_texture,
    "stadium_track_df.tga": track_texture,
    "stadium_grass_df.tga": lambda: flat_texture([58, 128, 48], 9, 3),
    "stadium_concrete_df.tga": lambda: flat_texture([128, 126, 120], 7, 6),
    "stadium_bleacher_df.tga": bleacher_texture,
    "stadium_wall_df.tga": wall_texture,
    "stadium_goal_df.tga": lambda: flat_texture([235, 205, 40], 4, 7, 32),
}

# ------------------------------------------------------------------ geometry


class Surface:
    def __init__(self, tex):
        self.tex, self.pos, self.nrm, self.uv, self.idx = tex, [], [], [], []

    def quad(self, p, uv, n=None):
        """p: 4 corners counter-clockwise seen from the front; uv: 4 pairs."""
        if n is None:
            a, b, c = np.array(p[0]), np.array(p[1]), np.array(p[2])
            n = np.cross(b - a, c - a)
            n = n / (np.linalg.norm(n) or 1)
        base = len(self.pos)
        for k in range(4):
            self.pos.append(tuple(float(v) for v in p[k]))
            self.nrm.append(tuple(float(v) for v in n))
            self.uv.append((float(uv[k][0]), float(uv[k][1])))
        self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]

    def tri(self, p, uv):
        a, b, c = (np.array(x) for x in p)
        n = np.cross(b - a, c - a); n = n / (np.linalg.norm(n) or 1)
        base = len(self.pos)
        for k in range(3):
            self.pos.append(tuple(float(v) for v in p[k])); self.nrm.append(tuple(float(v) for v in n))
            self.uv.append((float(uv[k][0]), float(uv[k][1])))
        self.idx += [base, base + 1, base + 2]


def P(x, y, z):
    return (CX + x, CY + y, Z0 + z)


def box(s, x0, y0, z0, x1, y1, z1, tile=4.0, faces="tnsew", coll=None):
    """Axis-aligned box; faces: t top, n +y, s -y, e +x, w -x (bottom never)."""
    def add(q, uv):
        s.quad(q, uv)
        if coll is not None:
            coll.append((q[0], q[1], q[2])); coll.append((q[0], q[2], q[3]))
    if "t" in faces:
        add([P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)],
            [(x0 / tile, y0 / tile), (x1 / tile, y0 / tile), (x1 / tile, y1 / tile), (x0 / tile, y1 / tile)])
    if "s" in faces:
        add([P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1)],
            [(x0 / tile, z0 / tile), (x1 / tile, z0 / tile), (x1 / tile, z1 / tile), (x0 / tile, z1 / tile)])
    if "n" in faces:
        add([P(x1, y1, z0), P(x0, y1, z0), P(x0, y1, z1), P(x1, y1, z1)],
            [(x1 / tile, z0 / tile), (x0 / tile, z0 / tile), (x0 / tile, z1 / tile), (x1 / tile, z1 / tile)])
    if "e" in faces:
        add([P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)],
            [(y0 / tile, z0 / tile), (y1 / tile, z0 / tile), (y1 / tile, z1 / tile), (y0 / tile, z1 / tile)])
    if "w" in faces:
        add([P(x0, y1, z0), P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1)],
            [(y1 / tile, z0 / tile), (y0 / tile, z0 / tile), (y0 / tile, z1 / tile), (y1 / tile, z1 / tile)])


# real dimensions: field 120 x 53.33 yd; 400 m track: straights 84.39 m, inner radius 36.5 m, 8 lanes of 1.22 m
FL, FW = 120 * 0.9144, 53.333 * 0.9144
STRAIGHT, R_IN, LANES = 84.39, 36.5, 8
R_OUT = R_IN + LANES * 1.22
HX, HY = 99.0, 72.0           # stadium wall (inner face)
WALL_H = 9.0


def oval_point(t, r):
    """t in [0, 1) around the track (counter-clockwise from the start of the north straight)."""
    half = STRAIGHT / 2
    per = 2 * STRAIGHT + 2 * math.pi * R_IN
    d = t * per
    if d < STRAIGHT:                              # north straight, west -> east
        return (-half + d, r)
    d -= STRAIGHT
    if d < math.pi * R_IN:                        # east bend
        a = math.pi / 2 - d / R_IN
        return (half + r * math.cos(a), r * math.sin(a))
    d -= math.pi * R_IN
    if d < STRAIGHT:                              # south straight, east -> west
        return (half - d, -r)
    d -= STRAIGHT
    a = -math.pi / 2 - d / R_IN                   # west bend
    return (-half + r * math.cos(a), r * math.sin(a))


def build_geometry():
    S = {k: Surface(k) for k in TEXTURES}
    coll = []
    # ground (concrete) inside the wall, collision for everything that is walked on at z = 0
    g = S["stadium_concrete_df.tga"]
    gq = [P(-HX, -HY, 0), P(HX, -HY, 0), P(HX, HY, 0), P(-HX, HY, 0)]
    g.quad(gq, [(-HX / 4, -HY / 4), (HX / 4, -HY / 4), (HX / 4, HY / 4), (-HX / 4, HY / 4)])
    coll += [(gq[0], gq[1], gq[2]), (gq[0], gq[2], gq[3])]
    # infield grass (inside the inner track edge), a few cm above the ground
    gr = S["stadium_grass_df.tga"]
    N = 160
    ctr = P(0, 0, 0.03)
    for k in range(N):
        a = oval_point(k / N, R_IN); b = oval_point((k + 1) / N, R_IN)
        # oval_point runs clockwise seen from above: b before a keeps the triangle facing up
        gr.tri([ctr, P(b[0], b[1], 0.03), P(a[0], a[1], 0.03)], [(0, 0), (b[0] / 4, b[1] / 4), (a[0] / 4, a[1] / 4)])
    # track ring
    tr = S["stadium_track_df.tga"]
    per = 2 * STRAIGHT + 2 * math.pi * R_IN
    for k in range(N):
        t0, t1 = k / N, (k + 1) / N
        i0, i1 = oval_point(t0, R_IN), oval_point(t1, R_IN)
        o0, o1 = oval_point(t0, R_OUT), oval_point(t1, R_OUT)
        u0, u1 = t0 * per / 10, t1 * per / 10
        tr.quad([P(i0[0], i0[1], 0.03), P(i1[0], i1[1], 0.03), P(o1[0], o1[1], 0.03), P(o0[0], o0[1], 0.03)],
                [(u0, 0), (u1, 0), (u1, 1), (u0, 1)], n=(0, 0, 1))  # counter-clockwise from above
    # football field on top of the infield
    f = S["stadium_field_df.tga"]
    f.quad([P(-FL / 2, -FW / 2, 0.06), P(FL / 2, -FW / 2, 0.06), P(FL / 2, FW / 2, 0.06), P(-FL / 2, FW / 2, 0.06)],
           [(0, 1), (1, 1), (1, 0), (0, 0)])
    # bleachers on both long sides: 12 rows, 0.45 m rise, 0.9 m deep, behind a 1.2 m front wall
    bl = S["stadium_bleacher_df.tga"]
    wa = S["stadium_wall_df.tga"]
    for side in (1, -1):
        y_front = R_OUT + 4.0
        box(wa, -46, min(side * y_front, side * (y_front + 0.4)), 0, 46, max(side * y_front, side * (y_front + 0.4)), 1.2,
            tile=3, faces="tnse" + "w" if True else "", coll=coll)
        for r in range(12):
            ya, yb = y_front + 0.4 + r * 0.9, y_front + 0.4 + (r + 1) * 0.9
            z = 0.8 + (r + 1) * 0.45
            box(bl, -45, min(side * ya, side * yb), 0, 45, max(side * ya, side * yb), z, tile=4,
                faces="t" + ("s" if side > 0 else "n") + "ew", coll=coll)
        yb = y_front + 0.4 + 12 * 0.9
        box(wa, -46, min(side * yb, side * (yb + 0.5)), 0, 46, max(side * yb, side * (yb + 0.5)), 0.8 + 12 * 0.45 + 2.0,
            tile=3, coll=coll)
    # goal posts at both ends (yellow): post, crossbar at 3.05 m, uprights 5.64 m apart, 9 m tall
    gp = S["stadium_goal_df.tga"]
    for side in (1, -1):
        x = side * (FL / 2 + 0.5)
        box(gp, x - 0.15, -0.15, 0, x + 0.15, 0.15, 3.05, tile=1, coll=coll)
        box(gp, x - 0.12, -2.95, 2.95, x + 0.12, 2.95, 3.15, tile=1, coll=coll)
        for yy in (-2.82, 2.82):
            box(gp, x - 0.08, yy - 0.08, 3.1, x + 0.08, yy + 0.08, 9.0, tile=1, coll=coll)
    # stadium wall: four inward-facing walls, 1 m thick, closing the map
    T = 1.0
    box(wa, -HX - T, HY, 0, HX + T, HY + T, WALL_H, tile=3, coll=coll)
    box(wa, -HX - T, -HY - T, 0, HX + T, -HY, WALL_H, tile=3, coll=coll)
    box(wa, HX, -HY, 0, HX + T, HY, WALL_H, tile=3, coll=coll)
    box(wa, -HX - T, -HY, 0, -HX, HY, WALL_H, tile=3, coll=coll)
    return S, coll

def minimap_image(size=1024):
    """Top-down map in the forest minimap's frame: LevelDef LevelWidth 200 m across the 1024 px image,
    centred on (CX, CY), +x right, +y up (lined up against tdm_collision_00 over a12_tdm_forest_minimap).
    Parchment like the game's maps, red team half on the left (-x), blue on the right."""
    px = (np.arange(size) + 0.5) / size
    X = (px - 0.5) * 200.0                            # world x - CX
    Y = (0.5 - px) * 200.0                            # world y - CY (image rows go down)
    x, y = np.meshgrid(X, Y)
    rng = np.random.default_rng(5)
    img = np.empty((size, size, 3)); img[:] = (226, 211, 178)
    img += rng.normal(0, 4, (size, size, 1))
    edge = np.clip((np.maximum(np.abs(x) - HX, np.abs(y) - HY)) / 12.0, 0, 1)[..., None]
    img = img * (1 - 0.25 * edge)
    def paint(mask, rgb, a=1.0):
        m = mask[..., None] * a
        img[:] = img * (1 - m) + np.array(rgb, float) * m
    inside = (np.abs(x) <= HX) & (np.abs(y) <= HY)
    paint((np.abs(x) <= HX + 1.5) & (np.abs(y) <= HY + 1.5) & ~inside, (70, 62, 55))      # stadium wall
    paint(inside, (176, 172, 164))                                                       # concrete
    dx = np.maximum(np.abs(x) - STRAIGHT / 2, 0); r = np.hypot(dx, y)
    paint(r <= R_OUT, (168, 64, 50))                                                     # track
    paint(r < R_IN, (84, 140, 64))                                                       # infield
    for k in range(1, LANES):
        paint(np.abs(r - (R_IN + 1.22 * k)) < 0.12, (235, 225, 210), 0.8)                # lane lines
    field = (np.abs(x) <= FL / 2) & (np.abs(y) <= FW / 2)
    paint(field, (72, 132, 56))
    paint(field & (x < -FL / 2 + 9.14), (170, 50, 45))                                   # red end zone
    paint(field & (x > FL / 2 - 9.14), (50, 70, 165))                                    # blue end zone
    for yd in range(10, 100, 10):
        xx = -FL / 2 + 9.14 + yd * 0.9144
        paint(field & (np.abs(x - xx) < 0.18), (235, 235, 230), 0.9)
    yf = R_OUT + 4.0
    stands = (np.abs(x) <= 46) & (np.abs(y) >= yf) & (np.abs(y) <= yf + 0.4 + 12 * 0.9 + 0.5)
    paint(stands, (120, 124, 132))                                                       # bleachers
    for k in range(13):
        paint(stands & (np.abs(np.abs(y) - (yf + 0.4 + k * 0.9)) < 0.1), (80, 82, 90), 0.8)
    for side in (1, -1):                                                                  # goal posts
        paint((np.abs(x - side * (FL / 2 + 0.5)) < 0.5) & (np.abs(y) < 3.0), (230, 200, 40))
    paint(inside & (x < 0), (200, 60, 50), 0.10)                                          # team halves
    paint(inside & (x >= 0), (60, 80, 200), 0.10)
    return np.clip(img, 0, 255).astype(np.uint8)


# ------------------------------------------------------------------ model / collision writers


def write_model(template_pig, S, out):
    model = pf.parse(open(template_pig, "rb").read())
    me = model["meshes"][0]
    base = me["subs"][0]
    subs, lo, hi = [], [1e9] * 3, [-1e9] * 3
    for tex, s in S.items():
        if not s.idx:
            continue
        sm = copy.deepcopy(base)
        n = len(s.pos)
        sm["textures"] = list(base["textures"]) if os.environ.get("STADIUM_ORIGTEX") else [tex]  # diagnostic
        # one material name per surface: sub-meshes that share a name share one material instance and all
        # drew the first one's texture (the whole stadium was football field)
        sm["material"] = "Stadium_" + tex.split("_")[1]
        sm["vcount"], sm["icount"] = n, len(s.idx)
        sm["idx"] = list(s.idx)
        sm["pos"], sm["pos4"] = list(s.pos), None
        sm["nrm"], sm["nrm4"] = list(s.nrm), None
        sm["uv"] = [list(s.uv) for _ in base["uv"]]
        if base.get("colour"):
            sm["colour"] = [base["colour"][0]] * n
        if base.get("ext"):
            ext = []
            for k in range(len(base["ext"])):
                arr = []
                for nv in s.nrm:
                    nn = np.array(nv); t = np.cross([0, 0, 1], nn) if abs(nn[2]) < 0.9 else np.array([1.0, 0, 0])
                    t = t / (np.linalg.norm(t) or 1); b = np.cross(nn, t)
                    vec = [nn, t, b][k % 3]
                    arr.append((float(vec[0]), float(vec[1]), float(vec[2]), 1.0))
                ext.append(arr)
            sm["ext"] = ext
        p = np.array(s.pos)
        l, h = p.min(0), p.max(0)
        sm["centre"] = list((l + h) / 2)
        lo = [min(a, b) for a, b in zip(lo, l)]; hi = [max(a, b) for a, b in zip(hi, h)]
        subs.append(sm)
    me["subs"] = subs
    me["bbox"] = lo + hi
    model["meshes"] = [me]
    # the template's mesh hangs under a pivot node translated to its spot in the forest (-736, 6, 5); the
    # stadium's vertices are already in level coordinates, so every node is reset to identity (otherwise the
    # whole stadium was drawn ~736 m away and only its collision, which has no node, was in place)
    for n in model["nodes"]:
        n["pos"], n["rot"], n["scale"] = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0]
    data = pf.write(model)
    pf.parse(data)
    open(out, "wb").write(data)
    return sum(len(s.pos) for s in S.values())


def write_coll(tris, out, mask=None):
    """.coll: s32 100, u32 nv, vec3[nv], u32 nn, vec3[nn] unit normals, u32 nt, nt x (u16 i0, i1, i2,
    u16 normal index, u32 surface mask). Every triangle has its own normal (the shipped files have nn == nt,
    winding counter-clockwise around it); the engine's ground probes use them: with no normals the player
    found no ground at the spawns and was dropped at the world origin, far from the stadium's model.
    Masks as in tdm_collision_00: 3 on floors (normal z ~ 1), 8 on walls and other surfaces."""
    verts, index = [], {}
    def vid(p):
        k = tuple(round(c, 4) for c in p)
        if k not in index:
            index[k] = len(verts); verts.append(k)
        return index[k]
    faces, normals = [], []
    for a, b, c in tris:
        n = np.cross(np.array(b) - np.array(a), np.array(c) - np.array(a))
        l = np.linalg.norm(n)
        if l < 1e-9:
            continue
        n = n / l
        faces.append((vid(a), vid(b), vid(c), len(normals), 3 if n[2] > 0.7 else 8))
        normals.append(tuple(float(x) for x in n))
    if len(verts) > 65535 or len(normals) > 65535:
        raise SystemExit("collision: too many vertices")
    b = bytearray(struct.pack("<iI", 100, len(verts)))
    for v in verts:
        b += struct.pack("<3f", *v)
    b += struct.pack("<I", len(normals))
    for n in normals:
        b += struct.pack("<3f", *n)
    b += struct.pack("<I", len(faces))
    for f in faces:
        b += struct.pack("<4HI", *f)
    open(out, "wb").write(b)
    return len(verts), len(faces)


def first_mask(coll_bytes):
    _, nv = struct.unpack_from("<iI", coll_bytes, 0)
    p = 8 + 12 * nv
    nn = struct.unpack_from("<I", coll_bytes, p)[0]; p += 4 + 12 * nn
    nt = struct.unpack_from("<I", coll_bytes, p)[0]; p += 4
    return struct.unpack_from("<4HI", coll_bytes, p)[4]

# ------------------------------------------------------------------ level edit


def edit_level(lvl):
    names = {t["id"]: t["name"] for t in lvl["templates"]}
    keep_deco = ("tdm_statickmesh_buildings.pig", "redplains_sky.pig", "ctf_sun.pig")  # stadium, sky, sun
    land_material = None
    for_material = []
    drop = {"Cover", "ParticleSystem"} | ({"FogArea"} if os.environ.get("STADIUM_NOFOG") else set())
    spawn_i = {"red": 0, "blue": 0}
    stats = {"dropped": 0, "spawns": 0, "flags": 0}

    def model_of(e):
        for p in e.get("params", []):
            if p.get("name") in ("Model", "MeshModel"):
                return p["values"][0].get("s", "")
        return ""

    def set_file(e, param, name):
        for p in e.get("params", []):
            if p.get("name") == param:
                p["values"][0]["s"] = name
                stats.setdefault("renamed", []).append(name)

    def team_of(e):
        for p in e.get("params", []):
            if p.get("name") == "TeamType":
                return p["values"][0].get("s", "")
        return ""

    def keep(e):
        tn = names.get(e["template"])
        if os.environ.get("STADIUM_KEEP_ALL"):  # diagnostic: keep the forest, only override the files
            return True
        if tn in drop:
            return False
        if tn == "Deco" and model_of(e) not in keep_deco and not ("Flag" in e["name"] and not e["name"].startswith("Deco_")):
            # Flag::PostInit looks up "Base_Red_Flag"/"Base_Blue_Flag" by name and calls into it without a NULL
            # check (crash at load); the flags' bases and invisible boxes stay and move with their flag
            return False
        if tn == "Collision" and model_of(e) != "tdm_collision_00.coll":
            return False
        return True

    def place(e):
        tn = names.get(e["template"])
        if os.environ.get("STADIUM_NO_MOVE"):  # diagnostic: leave every entity where the forest has it
            return
        if tn in ("Deco", "Flag") and "Flag" in e["name"] and not e["name"].startswith("Deco_") and "pos" in e:
            flag_parts.append(e)
        if tn == "Deco" and model_of(e) == "tdm_statickmesh_buildings.pig":
            for_material.append(e)
            set_file(e, "Model", "stadium.pig")  # own names: Canyon's files stay untouched
        if tn == "Collision" and model_of(e) == "tdm_collision_00.coll":
            set_file(e, "MeshModel", "stadium.coll")
        if tn == "LevelDef":
            # the forest's OnStart toggles its winter decoration groups (Disable/Hide "G_Winter", ...); those
            # groups still list the removed meshes and Group::GetEntity returned NULL (crash at load). The
            # stadium has no winter variant: same handler name, empty body (plain source is accepted).
            for p in e.get("params", []):
                if p.get("name") == "OnStart":
                    p["values"][0]["text"] = "function X%d_G()\nend\n" % e["id"]
                    stats["onstart"] = "replaced"
        if tn == "NPCSpawnArea":
            team = team_of(e) or ("red" if "red" in e["name"].lower() else "blue")
            k = spawn_i.get(team, 0); spawn_i[team] = k + 1
            side = -1 if team == "red" else 1
            e["pos"] = list(P(side * (FL / 2 + 6 + (k % 2) * 3), -12 + (k // 2) * 8, 0.5))
            stats["spawns"] += 1
        elif tn == "Flag":
            side = -1 if "red" in e["name"].lower() else 1
            e["_orig"] = list(e["pos"])
            e["pos"] = list(P(side * (FL / 2 - 4.5), 0, 0.1))
            stats["flags"] += 1

    def walk(folder):
        items = []
        for it in folder.get("items", []):
            if "e" in it:
                if not keep(it["e"]):
                    stats["dropped"] += 1
                    continue
                place(it["e"])
            for key in ("f", "folder"):  # sub-folders are stored under "f"
                if key in it:
                    walk(it[key])
            items.append(it)
        folder["items"] = items

    # the land model's material set (Deco "TDM_StaticMesh_Land_001") defines the "Ground" material the stadium
    # sub-meshes use (copied from that model); the buildings' set has no "Ground" and nothing was drawn
    def find_land(o):
        if isinstance(o, dict):
            if names.get(o.get("template")) == "Deco" and model_of(o) == "tdm_statickmesh_land_01.pig":
                for p in o.get("params", []):
                    if p.get("name") == "Materials":
                        return p["values"][0]["u"]
            for v in o.values():
                r = find_land(v)
                if r:
                    return r
        elif isinstance(o, list):
            for v in o:
                r = find_land(v)
                if r:
                    return r
        return None
    flag_parts = []
    land_material = find_land(lvl["files"][-1])
    # "TDM_StaticMesh_SelfIlumm" (diffuse_only.xml): the land's lightmapped material multiplies a baked
    # lightmap (TDM_LM_Land.tga, uv set 1) that does not belong to this geometry and drew as coloured noise
    land_material = 267098
    if os.environ.get("STADIUM_MATERIAL"):  # diagnostic: material entity id for the stadium Deco
        land_material = int(os.environ["STADIUM_MATERIAL"])
    walk(lvl["files"][-1]["folder"])
    # flag bases / boxes keep their offset from their team's Flag entity (orig positions recorded before the move)
    flags = {("red" if "red" in e["name"].lower() else "blue"): e for e in flag_parts
             if names.get(e["template"]) == "Flag"}
    for e in flag_parts:
        team = "red" if "red" in e["name"].lower() else "blue"
        f = flags.get(team)
        if f is None or e is f or "_orig" not in f:
            continue
        e["pos"] = [f["pos"][i] + e["pos"][i] - f["_orig"][i] for i in range(3)]
        stats["flag_parts"] = stats.get("flag_parts", 0) + 1
    for f in flags.values():
        f.pop("_orig", None)
    for e in for_material:
        for p in e.get("params", []):
            if p.get("name") == "Materials" and land_material:
                p["values"][0]["u"] = land_material
                stats["material"] = land_material

    # entity groups ("g" items: {name, members = entity ids}) must not list removed entities: the game walks
    # them (GS_GamePlay ctor, Lua Disable/Hide) and Group::GetEntity returns NULL for a missing id -> crash
    alive = set()
    def ids(o):
        if isinstance(o, dict):
            if "template" in o and "id" in o:
                alive.add(o["id"])
            for v in o.values():
                ids(v)
        elif isinstance(o, list):
            for v in o:
                ids(v)
    ids(lvl["files"])
    def groups(o):
        if isinstance(o, dict):
            g = o.get("g")
            if isinstance(g, dict) and "members" in g:
                before = len(g["members"])
                g["members"] = [m for m in g["members"] if m in alive]
                stats["group_members_removed"] = stats.get("group_members_removed", 0) + before - len(g["members"])
            for v in o.values():
                groups(v)
        elif isinstance(o, list):
            for v in o:
                groups(v)
    groups(lvl["files"])
    return stats

# ------------------------------------------------------------------ main


def main(obb, outdir):
    mod = os.path.join(outdir, "mod")
    for d in ("models", "collisions", "textures", "custommaps/stadium"):
        os.makedirs(os.path.join(mod, d), exist_ok=True)
    z = zipfile.ZipFile(obb)
    tmp = os.path.join(outdir, "orig")
    os.makedirs(tmp, exist_ok=True)
    for n in ("models/tdm_statickmesh_land_01.pig", "levels.pak", "sprites_android/a12_tdm_forest_minimap.tga"):
        open(os.path.join(tmp, os.path.basename(n)), "wb").write(z.read(n))
    # minimap: "stadium__<file>" (lower case) replaces <file> during stadium matches only (port: custom_maps.c)
    png = os.path.join(tmp, "stadium_minimap.png")
    tt.write_png(png, minimap_image())
    tt.build(png, os.path.join(mod, "custommaps", "stadium", "stadium__a12_tdm_forest_minimap.tga"),
             like=os.path.join(tmp, "a12_tdm_forest_minimap.tga"))
    for name, fn in TEXTURES.items():
        save_tex(os.path.join(mod, "textures", name), fn())
    S, coll = build_geometry()
    nv = write_model(os.path.join(tmp, "tdm_statickmesh_land_01.pig"), S, os.path.join(mod, "models", "stadium.pig"))
    cv, cf = write_coll(coll, os.path.join(mod, "collisions", "stadium.coll"))
    print(f"[stadium] model {nv} vertices; collision {cv} vertices, {cf} triangles (floors mask 3, walls 8)")
    # level: a12_tdm_forest.bclara out of levels.pak, edited, stored as the custom map's level
    bt = os.path.join(MOD_TOOLS, "bclara_tool.py")
    pk = zipfile.ZipFile(os.path.join(tmp, "levels.pak"))
    src = [n for n in pk.namelist() if n.lower().endswith("a12_tdm_forest.bclara")][0]
    open(os.path.join(tmp, "a12.bclara"), "wb").write(pk.read(src))
    # the helper tools are imported and called in-process (the packaged SD builder app has no separate python)
    import bclara_tool
    sys.path.insert(0, os.path.join(MOD_TOOLS, "..", "scripts"))
    import pack_mod
    import contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        bclara_tool.cmd_dump(os.path.join(tmp, "a12.bclara"), os.path.join(tmp, "a12.json"))
    lvl = json.load(open(os.path.join(tmp, "a12.json")))
    st = edit_level(lvl)
    print(f"[stadium] level: {st}")
    json.dump(lvl, open(os.path.join(tmp, "a12_stadium.json"), "w"))
    with contextlib.redirect_stdout(io.StringIO()):
        bclara_tool.cmd_load(os.path.join(tmp, "a12_stadium.json"), os.path.join(mod, "custommaps", "stadium", "stadium.bclara"))
    with open(os.path.join(mod, "custom_map.txt"), "w") as f:
        f.write("# custom multiplayer map (port: source/custom_maps.c)\n"
                "name=Stadium\n"
                "base=2\n"  # Canyon (levels/A12_TDM_Forest.bclara)
                "level=custommaps/stadium/stadium.bclara\n")
    pack_mod.pack(mod, os.path.join(outdir, "30_stadium.zip"))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
