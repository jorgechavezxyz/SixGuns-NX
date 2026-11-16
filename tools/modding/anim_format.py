"""Six Guns `.anim` animation reader (Animation::InitV100 0x1287f70 / Load 0x12880e0).

  python3 -I anim_format.py info  <file.anim>            summary of one animation
  python3 -I anim_format.py check <main.obb | folder>    parse every .anim, report how many end exactly at EOF
  python3 -I anim_format.py dump  <file.anim> [node]     per-key values of one node (or all nodes)

Layout (little endian; see docs/re/subsystems/engine_core.md 5.3):
  u32 fps; s16 nodeCount; nodeCount x (str name, u8 flags)   flags bit1 = rotations are 6-byte compressed
  s16 materialCount; x (str name, s16 texChannelCount)
  channel groups, each over all nodes: positions (vec3), rotations, scales (vec3); then per material the
  UV pos / rot / scale tracks of every texture channel.
  A track is `u16 n`; n == 0 -> one constant value follows, else `u16 time[n]` then `value[n]`.
  Time: low 15 bits << 3 = milliseconds. Top bit set on a key = hold (no interpolation towards the next key).
  Rotation key: 4 x s16 / 32767 (x y z w), or compressed 3 x s16 (see decompress_rot).
Quaternions are (x, y, z, w) in the engine's convention (the transpose of the usual one, same as the .pig
node rotations: use pig_format.quat_mat). Keys are interpolated with nlerp, positions/scales linearly.

Used as a module: parse(bytes) -> dict, sample(anim, node_index, ms) -> (pos, quat, scale).
"""
import math
import struct
import sys
import zipfile


class Truncated(Exception):
    pass


class _R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def take(self, fmt):
        n = struct.calcsize("<" + fmt)
        if self.p + n > len(self.b):
            raise Truncated(f"need {n} bytes at {self.p}, file is {len(self.b)}")
        v = struct.unpack_from("<" + fmt, self.b, self.p)
        self.p += n
        return v

    def st(self):
        n = self.take("H")[0]
        if self.p + n > len(self.b):
            raise Truncated("string")
        s = self.b[self.p:self.p + n].decode("latin-1")
        self.p += n
        return s


def decompress_rot(a, b, c):
    """DecompressRotation 0x12896f8 / 0x128b0dc: a is a 15-bit signed x (bits 15..1, scale 1/16384) with the
    sign of w in bit 0; b, c are s16 y, z (scale 1/32767); w = +-sqrt(1 - x^2 - y^2 - z^2), then normalised."""
    a &= 0xFFFF
    s = a >> 1                          # 15 bits
    if s & 0x4000:
        s -= 0x8000                      # sign-extend bit 14
    x = s * 6.103888e-05
    y = b * 3.051851e-05
    z = c * 3.051851e-05
    w = math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z))
    if not (a & 1):
        w = -w
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return (x / n, y / n, z / n, w / n)


def _rot(raw8):
    return tuple(v * 3.051851e-05 for v in raw8)


def parse(data):
    """Whole animation. Raises Truncated on short data; result['end'] is the parsed length (== len(data) when exact)."""
    r = _R(data)
    fps = r.take("I")[0]
    nn = r.take("h")[0]
    nodes = []
    for _ in range(nn):
        name = r.st()
        nodes.append({"name": name, "flags": r.take("B")[0]})
    nm = r.take("h")[0]
    mats = []
    for _ in range(nm):
        name = r.st()
        mats.append({"name": name, "tex": r.take("h")[0]})
    if nn < 0 or nm < 0:
        raise Truncated("negative counts")

    def track(fmt, conv):
        """-> ('const', value) or ('keys', times_raw, values). conv maps the unpacked tuple to a value."""
        n = r.take("H")[0]
        if n == 0:
            return ("const", conv(r.take(fmt)))
        t = list(r.take(f"{n}H"))
        size = struct.calcsize("<" + fmt)
        if r.p + n * size > len(r.b):
            raise Truncated("track data")
        vals = [conv(struct.unpack_from("<" + fmt, r.b, r.p + i * size)) for i in range(n)]
        r.p += n * size
        return ("keys", t, vals)

    for nd in nodes:
        nd["pos"] = track("3f", tuple)
    for nd in nodes:
        if nd["flags"] & 2:
            nd["rot"] = track("3h", lambda v: decompress_rot(*v))
        else:
            nd["rot"] = track("4h", _rot)
    for nd in nodes:
        nd["scale"] = track("3f", tuple)
    for m in mats:
        m["uvpos"] = [track("2h", lambda v: (v[0] / 4096, v[1] / 4096)) for _ in range(m["tex"])]
        m["uvrot"] = [track("h", lambda v: v[0] / 4096) for _ in range(m["tex"])]
        m["uvscale"] = [track("2h", lambda v: (v[0] / 4096, v[1] / 4096)) for _ in range(m["tex"])]
    dur = 0
    for nd in nodes:
        for k in ("pos", "rot", "scale"):
            if nd[k][0] == "keys":
                dur = max(dur, (nd[k][1][-1] & 0x7FFF) << 3)
    return {"fps": fps, "nodes": nodes, "materials": mats, "duration_ms": dur, "end": r.p, "size": len(data)}


def key_ms(t):
    return (t & 0x7FFF) << 3


def key_hold(t):
    return bool(t & 0x8000)


def _nlerp(q0, q1, t):
    d = sum(a * b for a, b in zip(q0, q1))
    s = -1.0 if d < 0 else 1.0
    q = [a + (s * b - a) * t for a, b in zip(q0, q1)]
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return tuple(v / n for v in q)


def sample_track(tr, ms, quat=False):
    if tr[0] == "const":
        return tr[1]
    times, vals = tr[1], tr[2]
    i = 0
    while i + 1 < len(times) and key_ms(times[i + 1]) <= ms:
        i += 1
    if i + 1 >= len(times) or key_hold(times[i]):
        return vals[i]
    t0, t1 = key_ms(times[i]), key_ms(times[i + 1])
    f = min(max((ms - t0) / (t1 - t0), 0.0), 1.0) if t1 > t0 else 0.0
    a, b = vals[i], vals[i + 1]
    return _nlerp(a, b, f) if quat else tuple(x + (y - x) * f for x, y in zip(a, b))


def sample(anim, ni, ms):
    n = anim["nodes"][ni]
    return sample_track(n["pos"], ms), sample_track(n["rot"], ms, True), sample_track(n["scale"], ms)


def iter_obb(path):
    with zipfile.ZipFile(path) as z:
        for name in sorted(z.namelist()):
            if name.lower().endswith(".anim"):
                yield name, z.read(name)


def iter_dir(path):
    import os
    for root, _d, files in os.walk(path):
        for f in sorted(files):
            if f.lower().endswith(".anim"):
                p = os.path.join(root, f)
                yield p, open(p, "rb").read()


def cmd_info(path):
    a = parse(open(path, "rb").read())
    print(f"{path}: fps {a['fps']}, {len(a['nodes'])} nodes, {len(a['materials'])} materials, "
          f"duration {a['duration_ms']} ms ({a['duration_ms'] * a['fps'] / 1000:.1f} frames), parsed {a['end']}/{a['size']} bytes")
    print(f"  compressed rotations: {sum(1 for n in a['nodes'] if n['flags'] & 2)} nodes; "
          f"hold keys: {sum(sum(key_hold(t) for t in n[k][1]) for n in a['nodes'] for k in ('pos', 'rot', 'scale') if n[k][0] == 'keys')}")
    for n in a["nodes"]:
        d = " ".join(f"{k}={'const' if n[k][0] == 'const' else len(n[k][1])}" for k in ("pos", "rot", "scale"))
        print(f"  {n['name']:<28} flags {n['flags']} {d}")


def cmd_check(path):
    import os
    it = iter_dir(path) if os.path.isdir(path) else iter_obb(path)
    total = ok = 0
    bad = []
    qerr = 0.0
    for name, data in it:
        total += 1
        try:
            a = parse(data)
        except (Truncated, struct.error, UnicodeDecodeError) as e:
            bad.append((name, f"error: {e}"))
            continue
        if a["end"] == a["size"]:
            ok += 1
        else:
            bad.append((name, f"parsed {a['end']} of {a['size']} bytes"))
        for n in a["nodes"]:
            qs = n["rot"][2] if n["rot"][0] == "keys" else [n["rot"][1]]
            for q in qs:
                qerr = max(qerr, abs(math.sqrt(sum(v * v for v in q)) - 1.0))
    print(f"{ok}/{total} parse to their exact end; max |quat length - 1| = {qerr:.4f}")
    for name, why in bad:
        print("  FAIL", name, why)


def cmd_dump(path, only=None):
    a = parse(open(path, "rb").read())
    for n in a["nodes"]:
        if only and n["name"] != only:
            continue
        print(n["name"])
        for k in ("pos", "rot", "scale"):
            tr = n[k]
            if tr[0] == "const":
                print(f"  {k}: const {tuple(round(v, 4) for v in tr[1])}")
            else:
                for t, v in zip(tr[1], tr[2]):
                    print(f"  {k} {key_ms(t):5d} ms{' H' if key_hold(t) else '  '} {tuple(round(x, 4) for x in v)}")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "info":
        cmd_info(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        cmd_check(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "dump":
        cmd_dump(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    else:
        sys.exit(__doc__)
