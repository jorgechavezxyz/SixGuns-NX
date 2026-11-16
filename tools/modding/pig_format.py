#!/usr/bin/env python3
"""Six Guns `.pig` models (characters/*.pig, models/*.pig in the OBB): reader.

  python3 -I pig_format.py info <model.pig>           nodes, meshes, sub-meshes, textures
  python3 -I pig_format.py obj  <model.pig> <out.obj>  first LOD as Wavefront OBJ (+ .mtl) for any 3D tool
  python3 -I pig_format.py check <obb or folder>       parse + write every .pig, report byte-identical

Layout from the engine loaders (ModelBase::Load 0x124ccf0, Mesh::LoadV100 0x128e878, SubMesh::LoadV100
0x125751c, SkinnedSubMesh::LoadV100 0x1255e58, LoadSkinData 0x1256168); documented in
docs/re/subsystems/engine_core.md §5.2 (all 1,352 models in the OBB parse byte-exact). Plain Python, no
dependencies; the Blender importer (blender_import_pig.py) uses this module.

Skinned sub-meshes carry no positions in the geometry blob: their bind-pose positions/normals follow the
blob in the skin extension (indexed through vertexRemap), and the bone table after that gives per-bone
influences (vertex index, weight-scaled offset in bone space, weight).
"""
import struct
import sys


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v

    def u8(self): return self.take("B")[0]
    def s8(self): return self.take("b")[0]
    def u16(self): return self.take("H")[0]
    def s16(self): return self.take("h")[0]
    def u32(self): return self.take("I")[0]
    def s32(self): return self.take("i")[0]
    def f(self, n): return list(self.take(f"{n}f"))

    def st(self):
        n = self.u16()
        s = self.b[self.p:self.p + n].decode("latin-1")
        self.p += n
        return s


def _arr(d, p, count, fmt):
    """count records of struct fmt, then the u16 pad length + pad. Returns (list, new p)."""
    size = struct.calcsize("<" + fmt)
    out = [struct.unpack_from("<" + fmt, d, p + i * size) for i in range(count)]
    p += count * size
    pad = struct.unpack_from("<H", d, p)[0]
    return out, p + 2 + pad


def parse(data):
    """Whole model as dicts. Keeps everything write() needs to rebuild the file byte for byte."""
    r = R(data)
    assert r.s32() == 100, "not a v100 .pig"
    nodes = []
    for _ in range(r.u16()):
        assert r.s32() == 100
        name = r.st()
        ntype = r.s8()
        parent = r.s16()
        pos, rot, scale = r.f(3), r.f(4), r.f(3)
        props = [(r.st(), r.st()) for _ in range(r.u16())]
        nodes.append(dict(name=name, type=ntype, parent=parent, pos=pos, rot=rot, scale=scale, props=props))
    flag = r.u8()
    meshes, groups = [], []
    for gi in range(r.u16()):
        assert r.s32() == 100
        groups.append(dict(lod_info=r.s32()))
        for _ in range(r.u16()):
            lod = r.s8()
            assert r.s32() == 100
            skinned = r.u8()
            mask = r.u32()
            bbox = r.f(6)
            subs = []
            for _ in range(r.u16()):
                assert r.s32() == 100
                centre = r.f(3)
                vc, ic, extra = r.u16(), r.u16(), r.u16()
                material = r.st()
                textures = [r.st() for _ in range(r.u16())]
                size = r.u32()
                d = data[r.p:r.p + size]
                r.p += size
                sm = dict(material=material, textures=textures, vcount=vc, icount=ic, extra=extra, centre=centre,
                          pos=None, nrm=None, pos4=None, nrm4=None, ext=[], colour=None, uv=[], uvtable=None,
                          idx=None, bones=[])
                p = 2 + struct.unpack_from("<H", d, 0)[0]
                if not skinned:
                    if mask & 1:
                        sm["pos4"], p = _arr(d, p, vc, "4f")
                        sm["pos"] = [v[:3] for v in sm["pos4"]]
                    if mask & 2:
                        sm["nrm4"], p = _arr(d, p, vc, "4f")
                        sm["nrm"] = [v[:3] for v in sm["nrm4"]]
                    if mask & 0x1004 and extra:
                        for _ in range(extra):
                            a, p = _arr(d, p, vc, "4f")
                            sm["ext"].append(a)
                if mask & 8:
                    sm["colour"], p = _arr(d, p, vc, "4B")
                table = struct.unpack_from("<8I", d, p)
                sm["uvtable"] = list(table)
                p += 32
                for i in range(8):
                    if mask & (0x10 << i) and table[i] == i:
                        uv, p = _arr(d, p, vc, "2f")
                        sm["uv"].append(uv)
                sm["idx"] = list(struct.unpack_from(f"<{ic}H", d, p))
                p += ic * 2
                assert p == size, f"sub-mesh blob: read {p} of {size}"
                if skinned:
                    sm["skin_unk"] = r.u16()
                    n = r.u16()
                    remap = list(r.take(f"{n}H"))
                    bind = {}
                    for bit, key in ((1, "pos"), (2, "nrm"), (4, "tan"), (0x1000, "x")):
                        if mask & bit:
                            bind[key] = [tuple(r.f(3)) for _ in range(n)]
                    sm["remap"], sm["bind"] = remap, bind
                    for _ in range(r.u16()):
                        node = r.u16()
                        infl, recs = [], []
                        for _ in range(r.u16()):
                            vi = r.u16()
                            off = r.f(3)
                            noff = toff = None
                            if mask & 2:
                                noff = r.f(3)  # normal offset
                                toff = r.f(3)  # tangent offset (present in the record even if unused)
                            w = r.f(1)[0]
                            infl.append((vi, off, w))
                            recs.append((vi, off, noff, toff, w))
                        sm["bones"].append(dict(node=node, influences=infl, records=recs))
                subs.append(sm)
            meshes.append(dict(lod=lod, skinned=skinned, mask=mask, bbox=bbox, subs=subs, group=gi))
    assert r.p == len(data), f"trailing bytes: {len(data) - r.p}"
    return dict(nodes=nodes, flag=flag, meshes=meshes, groups=groups)


# ---------------------------------------------------------------- writer

class W:
    def __init__(self):
        self.b = bytearray()

    def put(self, fmt, *v): self.b += struct.pack("<" + fmt, *v)
    def st(self, s):
        e = s.encode("latin-1")
        self.put("H", len(e)); self.b += e


def _pad_to16(w, base):
    """u16 pad length + zero bytes so that the next data starts 16-aligned in the file (base = blob start)."""
    here = base + len(w.b) + 2
    n = (-here) % 16
    w.put("H", n)
    w.b += bytes(n)


def write(model):
    """Serialise a model dict (as parse() returns, possibly edited) back to .pig bytes. Counts (vertices,
    indices, sub-meshes, bones) are taken from the data, so edited geometry just needs consistent arrays."""
    w = W()
    w.put("i", 100)
    w.put("H", len(model["nodes"]))
    for n in model["nodes"]:
        w.put("i", 100); w.st(n["name"]); w.put("b", n["type"]); w.put("h", n["parent"])
        w.put("3f", *n["pos"]); w.put("4f", *n["rot"]); w.put("3f", *n["scale"])
        w.put("H", len(n["props"]))
        for k, v in n["props"]:
            w.st(k); w.st(v)
    w.put("B", model["flag"])
    w.put("H", len(model["groups"]))
    for gi, g in enumerate(model["groups"]):
        ms = [m for m in model["meshes"] if m["group"] == gi]
        w.put("i", 100); w.put("i", g["lod_info"]); w.put("H", len(ms))
        for me in ms:
            mask, sk = me["mask"], me["skinned"]
            w.put("b", me["lod"]); w.put("i", 100); w.put("B", sk); w.put("I", mask); w.put("6f", *me["bbox"])
            w.put("H", len(me["subs"]))
            for sm in me["subs"]:
                vc = len(sm["uv"][0]) if sm["uv"] else sm["vcount"]
                if not sk and sm.get("pos"):
                    vc = len(sm["pos"])
                w.put("i", 100); w.put("3f", *sm["centre"])
                w.put("H", vc); w.put("H", len(sm["idx"])); w.put("H", len(sm["ext"]) if not sk else sm.get("extra", 0))
                w.st(sm["material"]); w.put("H", len(sm["textures"]))
                for t in sm["textures"]:
                    w.st(t)
                blob = W()
                base = len(w.b) + 4  # blob starts after the u32 size
                _pad_to16(blob, base)
                if not sk:
                    if mask & 1:
                        p4 = sm.get("pos4") if sm.get("pos4") and len(sm["pos4"]) == len(sm["pos"]) and \
                            all(tuple(a[:3]) == tuple(b) for a, b in zip(sm["pos4"], sm["pos"])) else [tuple(v) + (1.0,) for v in sm["pos"]]
                        for v in p4:
                            blob.put("4f", *v)
                        _pad_to16(blob, base)
                    if mask & 2:
                        n4 = sm.get("nrm4") if sm.get("nrm4") and len(sm["nrm4"]) == vc and \
                            all(tuple(a[:3]) == tuple(b) for a, b in zip(sm["nrm4"], sm["nrm"])) else [tuple(v) + (1.0,) for v in sm["nrm"]]
                        for v in n4:
                            blob.put("4f", *v)
                        _pad_to16(blob, base)
                    if mask & 0x1004 and sm["ext"]:
                        for a in sm["ext"]:
                            for v in a:
                                blob.put("4f", *v)
                            _pad_to16(blob, base)
                if mask & 8:
                    for c in sm["colour"]:
                        blob.put("4B", *c)
                    _pad_to16(blob, base)
                blob.put("8I", *sm["uvtable"])
                k = 0
                for i in range(8):
                    if mask & (0x10 << i) and sm["uvtable"][i] == i:
                        for t in sm["uv"][k]:
                            blob.put("2f", *t)
                        k += 1
                        _pad_to16(blob, base)
                blob.put(f"{len(sm['idx'])}H", *sm["idx"])
                w.put("I", len(blob.b))
                w.b += blob.b
                if sk:
                    w.put("H", sm.get("skin_unk", 0))
                    remap = sm.get("remap", [])
                    w.put("H", len(remap))
                    if remap:
                        w.put(f"{len(remap)}H", *remap)
                    for bit, key in ((1, "pos"), (2, "nrm"), (4, "tan"), (0x1000, "x")):
                        if mask & bit:
                            for v in sm.get("bind", {}).get(key, []):
                                w.put("3f", *v)
                    w.put("H", len(sm["bones"]))
                    for b in sm["bones"]:
                        recs = b.get("records") or [(vi, off, (0, 0, 0), (0, 0, 0), wt) for vi, off, wt in b["influences"]]
                        w.put("H", b["node"]); w.put("H", len(recs))
                        for vi, off, noff, toff, wt in recs:
                            w.put("H", vi); w.put("3f", *off)
                            if mask & 2:
                                w.put("3f", *(noff or (0, 0, 0))); w.put("3f", *(toff or (0, 0, 0)))
                            w.put("f", wt)
    return bytes(w.b)


# ---------------------------------------------------------------- transforms

def quat_mat(q):
    """Rotation matrix of a stored node quaternion (x, y, z, w). The engine's convention is the transpose
    of the usual one (checked: rebuilt T-pose arm span equals the mesh bbox only this way)."""
    x, y, z, w = q
    x, y, z = -x, -y, -z
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def node_world(nodes):
    """World 3x4 matrices of all nodes (local = T * R * S, parent first)."""
    out = [None] * len(nodes)

    def mat(i):
        if out[i] is not None:
            return out[i]
        n = nodes[i]
        R3 = quat_mat(n["rot"])
        L = [[R3[r][c] * n["scale"][c] for c in range(3)] + [n["pos"][r]] for r in range(3)]
        if n["parent"] >= 0:
            P = mat(n["parent"])
            L = [[sum(P[r][k] * L[k][c] for k in range(3)) + (P[r][3] if c == 3 else 0) for c in range(4)]
                 for r in range(3)]
        out[i] = L
        return L
    for i in range(len(nodes)):
        mat(i)
    return out


def skinned_positions(model, sm):
    """Bind-pose positions of a skinned sub-mesh, rebuilt from the bone table: sum over influences of
    weight * (boneWorld * offset). The file stores offsets unscaled (the engine pre-multiplies them by the
    weight when it builds its runtime records); checked: with this rule every vertex of outlawtophat.pig
    lies inside its mesh's stored bounding box, with pre-scaled offsets only 89% do."""
    world = node_world(model["nodes"])
    pos = [[0.0, 0.0, 0.0] for _ in range(sm["vcount"])]
    for b in sm["bones"]:
        M = world[b["node"]]
        for vi, off, _w in b["influences"]:
            for r in range(3):
                pos[vi][r] += _w * (M[r][0] * off[0] + M[r][1] * off[1] + M[r][2] * off[2] + M[r][3])
    return pos


def positions(model, sm, skinned):
    if not skinned:
        return sm["pos"]
    return skinned_positions(model, sm)


# ---------------------------------------------------------------- commands

def cmd_info(path):
    m = parse(open(path, "rb").read())
    print(f"{path}: {len(m['nodes'])} nodes, {len(m['meshes'])} meshes")
    for me in m["meshes"]:
        print(f"  mesh lod {me['lod']} skinned {me['skinned']} mask {me['mask']:#x} sub-meshes {len(me['subs'])}")
        for sm in me["subs"]:
            print(f"    {sm['material']}: {sm['vcount']} verts, {sm['icount'] // 3} tris, uv sets {len(sm['uv'])}, "
                  f"bones {len(sm['bones'])}, textures {', '.join(sm['textures'])}")


def cmd_obj(path, out):
    m = parse(open(path, "rb").read())
    base = 1
    mtl = out[:-4] + ".mtl"
    with open(out, "w") as f, open(mtl, "w") as fm:
        f.write(f"mtllib {mtl.split('/')[-1]}\n")
        lod0 = min(me["lod"] for me in m["meshes"])
        for mi, me in enumerate(x for x in m["meshes"] if x["lod"] == lod0):
            for si, sm in enumerate(me["subs"]):
                pos = positions(m, sm, me["skinned"])
                name = f"m{mi}_s{si}_{sm['material']}"
                fm.write(f"newmtl {name}\n")
                if sm["textures"]:
                    fm.write(f"map_Kd {sm['textures'][0].replace('.tga', '.png')}\n")
                f.write(f"o {name}\nusemtl {name}\n")
                for v in pos:
                    f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
                uv = sm["uv"][0] if sm["uv"] else None
                if uv:
                    for t in uv:
                        f.write(f"vt {t[0]:.5f} {1 - t[1]:.5f}\n")
                idx = sm["idx"]
                for k in range(0, len(idx) - 2, 3):
                    a, b, c = idx[k] + base, idx[k + 1] + base, idx[k + 2] + base
                    f.write(f"f {a}/{a} {b}/{b} {c}/{c}\n" if uv else f"f {a} {b} {c}\n")
                base += len(pos)
    print(f"{out} written ({base - 1} vertices)")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "info":
        cmd_info(a[1])
    elif len(a) >= 3 and a[0] == "obj":
        cmd_obj(a[1], a[2])
    elif len(a) >= 2 and a[0] == "check":
        import zipfile
        same = diff = 0
        z = zipfile.ZipFile(a[1])
        for n in z.namelist():
            if n.endswith(".pig"):
                d = z.read(n)
                ok = write(parse(d)) == d
                same += ok
                diff += not ok
                if not ok and diff <= 5:
                    print("DIFFERS:", n)
        print(f"{same} identical, {diff} different")
    else:
        sys.exit(__doc__)
