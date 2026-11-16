#!/usr/bin/env python3
"""Six Guns textures (`textures*/<name>.tga` in the OBBs): to PNG and back.

  PY=/Applications/Blender.app/Contents/Resources/5.2/python/bin/python3.13   # any Python with numpy
  $PY -I texture_tool.py info  <tex.tga>
  $PY -I texture_tool.py topng <tex.tga> <out.png>                 base level; RGBA if it has alpha
  $PY -I texture_tool.py build <in.png> <out.tga> [--like <original.tga>]

Despite the .tga name the files are PVR v2 textures, ETC1 compressed (pixel format 0x36), with a full
mip chain. Two containers exist in the OBBs:
  - a bare PVR file (52-byte header, "PVR!" tag), e.g. textures_chars/TopHat_Df.tga;
  - a stored zip holding `rgb.pvr` and optionally `alpha.pvr` (alpha as a grey ETC1 image).
`build` writes the same container as `--like` (default: bare PVR, or zip+alpha when the PNG has
transparency), ETC1-encodes every mip level (box-filtered) and keeps the original's size unless the
PNG differs (power of two needed). Loader: pig::video::TextureLoader / GLES20Texture::Load
(docs/re/subsystems/engine_save.md); the port decodes ETC1 to RGB565 at upload (gl_debug.c).
To get a texture into the game, put it in a mod folder at its OBB path and pack it
(tools/scripts/pack_mod.py), see tools/modding/README.md.
"""
import io
import struct
import sys
import zipfile
import zlib

import numpy as np

TABLES = np.array([[2, 8], [5, 17], [9, 29], [13, 42], [18, 60], [24, 80], [33, 106], [47, 183]], np.int32)
# pixel index (msb<<1|lsb) -> modifier: 0:+a 1:+b 2:-a 3:-b
MODS = np.stack([TABLES[:, 0], TABLES[:, 1], -TABLES[:, 0], -TABLES[:, 1]], 1)  # (8,4)
PVR_ETC1 = 0x36
FLAG_MIPMAP = 0x100

# ---------------------------------------------------------------- PVR container


def parse_pvr(b):
    h = struct.unpack_from("<13I", b, 0)
    if h[11] != 0x21525650:
        raise SystemExit("not a PVR v2 texture")
    hdr = dict(size=h[0], height=h[1], width=h[2], mips=h[3], flags=h[4], datasize=h[5], bpp=h[6],
               masks=h[7:11], nsurf=h[12])
    if hdr["flags"] & 0xFF != PVR_ETC1:
        raise SystemExit(f"pixel format {hdr['flags'] & 0xff:#x} not supported (only ETC1 0x36)")
    return hdr, b[h[0]:h[0] + h[5]]


def make_pvr(w, h, levels_data, like=None):
    data = b"".join(levels_data)
    flags = (like["flags"] if like else (FLAG_MIPMAP | PVR_ETC1))
    masks = like["masks"] if like else (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    hdr = struct.pack("<13I", 52, h, w, len(levels_data) - 1, flags, len(data), 4, *masks, 0x21525650, 1)
    return hdr + data


def read_texture(path):
    """(rgb uint8 HxWx3, alpha uint8 HxW or None, info dict)"""
    b = open(path, "rb").read()
    if b[:2] == b"PK":
        z = zipfile.ZipFile(io.BytesIO(b))
        hdr, data = parse_pvr(z.read("rgb.pvr"))
        rgb = decode_etc1(data, hdr["width"], hdr["height"])
        alpha = None
        if "alpha.pvr" in z.namelist():
            ah, ad = parse_pvr(z.read("alpha.pvr"))
            alpha = decode_etc1(ad, ah["width"], ah["height"])[:, :, 1]
        return rgb, alpha, dict(container="zip", hdr=hdr, names=z.namelist())
    hdr, data = parse_pvr(b)
    return decode_etc1(data, hdr["width"], hdr["height"]), None, dict(container="pvr", hdr=hdr)


# ---------------------------------------------------------------- ETC1


def _ext4(v):
    return (v << 4) | v


def _ext5(v):
    return (v << 3) | (v >> 2)


def decode_etc1(data, w, h):
    """Base level only."""
    bw, bh = max(w // 4, 1), max(h // 4, 1)
    n = bw * bh
    blk = np.frombuffer(data[:n * 8], np.uint8).reshape(n, 8).astype(np.int32)
    diff = (blk[:, 3] >> 1) & 1
    flip = blk[:, 3] & 1
    t1, t2 = blk[:, 3] >> 5, (blk[:, 3] >> 2) & 7
    base1 = np.zeros((n, 3), np.int32)
    base2 = np.zeros((n, 3), np.int32)
    for c in range(3):
        v = blk[:, c]
        ind1, ind2 = _ext4(v >> 4), _ext4(v & 15)
        b5 = v >> 3
        d = (v & 7) - ((v & 4) << 1)  # signed 3-bit
        dif1, dif2 = _ext5(b5), _ext5(np.clip(b5 + d, 0, 31))
        base1[:, c] = np.where(diff == 1, dif1, ind1)
        base2[:, c] = np.where(diff == 1, dif2, ind2)
    msb = (blk[:, 4] << 8) | blk[:, 5]
    lsb = (blk[:, 6] << 8) | blk[:, 7]
    out = np.zeros((n, 4, 4, 3), np.int32)  # [block, y, x, c]
    for x in range(4):
        for y in range(4):
            i = x * 4 + y
            idx = (((msb >> i) & 1) << 1) | ((lsb >> i) & 1)
            second = np.where(flip == 1, y >= 2, x >= 2)
            base = np.where(second[:, None], base2, base1)
            tab = np.where(second, t2, t1)
            mod = MODS[tab, idx]
            out[:, y, x, :] = np.clip(base + mod[:, None], 0, 255)
    img = out.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)
    return img[:h, :w].astype(np.uint8)


def encode_etc1(img):
    """img HxWx3 uint8 (H, W multiples of 4, or padded) -> ETC1 bytes. Tries both flips, individual
    and differential modes, all 8 tables per half; base colour = half average (quantised)."""
    h, w = img.shape[:2]
    ph, pw = max((h + 3) // 4 * 4, 4), max((w + 3) // 4 * 4, 4)
    pad = np.zeros((ph, pw, 3), np.int32)
    pad[:h, :w] = img
    if h < ph:
        pad[h:, :w] = pad[h - 1:h, :w]
    if w < pw:
        pad[:, w:] = pad[:, w - 1:w]
    bh, bw = ph // 4, pw // 4
    px = pad.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 3, 1, 4).reshape(-1, 4, 4, 3)  # [blk, x, y, c]
    n = px.shape[0]
    # halves: flip 0 -> x<2 / x>=2 ; flip 1 -> y<2 / y>=2. Pixel order inside a half: 8 pixels.
    halves = {
        0: (px[:, :2, :, :].reshape(n, 8, 3), px[:, 2:, :, :].reshape(n, 8, 3)),
        1: (px[:, :, :2, :].reshape(n, 8, 3), px[:, :, 2:, :].reshape(n, 8, 3)),
    }
    best_err = np.full(n, np.inf)
    best = None
    for flip in (0, 1):
        a, b = halves[flip]
        avg_a, avg_b = a.mean(1), b.mean(1)
        # individual mode: 4-bit each
        q4a = np.clip(np.rint(avg_a / 17), 0, 15).astype(np.int32)
        q4b = np.clip(np.rint(avg_b / 17), 0, 15).astype(np.int32)
        # differential: 5-bit, delta in [-4, 3]
        q5a = np.clip(np.rint(avg_a * 31 / 255), 0, 31).astype(np.int32)
        q5b = np.clip(np.rint(avg_b * 31 / 255), 0, 31).astype(np.int32)
        dlt = np.clip(q5b - q5a, -4, 3)
        q5b = q5a + dlt
        for mode in (0, 1):
            if mode == 0:
                ca, cb = _ext4(q4a), _ext4(q4b)
            else:
                ca, cb = _ext5(q5a), _ext5(q5b)
            res = []
            for half, base in ((a, ca), (b, cb)):
                # candidates: (n, 8 tables, 4 mods, 3) colours
                cand = np.clip(base[:, None, None, :] + MODS[None, :, :, None], 0, 255)
                e = ((half[:, None, None, :, :] - cand[:, :, :, None, :]) ** 2).sum(-1)  # n,8,4,8px
                emin = e.min(2)  # n,8,8
                idx = e.argmin(2)  # n,8,8
                terr = emin.sum(2)  # n,8
                tbest = terr.argmin(1)
                res.append((terr[np.arange(n), tbest], tbest, idx[np.arange(n), tbest]))
            err = res[0][0] + res[1][0]
            better = err < best_err
            if mode == 1:
                better &= True
            if best is None:
                best = dict(flip=np.zeros(n, np.int32), mode=np.zeros(n, np.int32),
                            c1=np.zeros((n, 3), np.int32), c2=np.zeros((n, 3), np.int32),
                            t1=np.zeros(n, np.int32), t2=np.zeros(n, np.int32),
                            i1=np.zeros((n, 8), np.int32), i2=np.zeros((n, 8), np.int32))
            best_err = np.where(better, err, best_err)
            best["flip"][better] = flip
            best["mode"][better] = mode
            if mode == 0:
                best["c1"][better], best["c2"][better] = q4a[better], q4b[better]
            else:
                best["c1"][better], best["c2"][better] = q5a[better], dlt[better]
            best["t1"][better], best["t2"][better] = res[0][1][better], res[1][1][better]
            best["i1"][better], best["i2"][better] = res[0][2][better], res[1][2][better]
    # pack
    out = np.zeros((n, 8), np.uint8)
    for c in range(3):
        ind = (best["c1"][:, c] << 4) | best["c2"][:, c]
        dif = (best["c1"][:, c] << 3) | (best["c2"][:, c] & 7)
        out[:, c] = np.where(best["mode"] == 1, dif, ind)
    out[:, 3] = (best["t1"] << 5) | (best["t2"] << 2) | (best["mode"] << 1) | best["flip"]
    msb = np.zeros(n, np.int32)
    lsb = np.zeros(n, np.int32)
    # map half-pixel order back to block pixel index i = x*4+y
    for flip in (0, 1):
        sel = best["flip"] == flip
        for k in range(8):
            for half_no, key in ((0, "i1"), (1, "i2")):
                if flip == 0:
                    x, y = half_no * 2 + k // 4, k % 4
                else:
                    x, y = k // 2, half_no * 2 + k % 2
                i = x * 4 + y
                v = best[key][:, k]
                msb[sel] |= ((v[sel] >> 1) & 1) << i
                lsb[sel] |= (v[sel] & 1) << i
    out[:, 4], out[:, 5] = msb >> 8, msb & 255
    out[:, 6], out[:, 7] = lsb >> 8, lsb & 255
    return out.tobytes()


def mip_chain(img, levels):
    out = [img]
    cur = img.astype(np.float32)
    for _ in range(levels - 1):
        h, w = cur.shape[:2]
        nh, nw = max(h // 2, 1), max(w // 2, 1)
        cur = cur[:nh * 2 if h > 1 else 1, :nw * 2 if w > 1 else 1]
        if h > 1:
            cur = (cur[0::2] + cur[1::2]) / 2
        if w > 1:
            cur = (cur[:, 0::2] + cur[:, 1::2]) / 2
        out.append(np.clip(np.rint(cur), 0, 255).astype(np.uint8))
    return out


# ---------------------------------------------------------------- PNG (8-bit, non-interlaced)


def write_png(path, arr):
    h, w = arr.shape[:2]
    ch = 1 if arr.ndim == 2 else arr.shape[2]
    ctype = {1: 0, 3: 2, 4: 6}[ch]
    raw = b"".join(b"\0" + arr[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def read_png(path):
    b = open(path, "rb").read()
    if b[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    p, idat, plte, trns = 8, b"", None, None
    while p < len(b):
        ln, t = struct.unpack_from(">I4s", b, p)
        d = b[p + 8:p + 8 + ln]
        if t == b"IHDR":
            w, h, depth, ctype, _c, _f, inter = struct.unpack(">IIBBBBB", d)
        elif t == b"IDAT":
            idat += d
        elif t == b"PLTE":
            plte = np.frombuffer(d, np.uint8).reshape(-1, 3)
        elif t == b"tRNS":
            trns = np.frombuffer(d, np.uint8)
        p += 12 + ln
    if depth != 8 or inter:
        raise SystemExit("PNG must be 8-bit per channel and not interlaced (re-save it from any editor)")
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = np.frombuffer(zlib.decompress(idat), np.uint8)
    stride = w * ch
    img = np.zeros((h, stride), np.int32)
    prev = np.zeros(stride, np.int32)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        else:
            cur = np.zeros(stride, np.int32)
            for x in range(stride):
                left = cur[x - ch] if x >= ch else 0
                up = prev[x]
                ul = prev[x - ch] if x >= ch else 0
                if f == 1:
                    pr = left
                elif f == 3:
                    pr = (left + up) >> 1
                else:
                    pa, pb, pc = abs(up - ul), abs(left - ul), abs(left + up - 2 * ul)
                    pr = left if pa <= pb and pa <= pc else up if pb <= pc else ul
                cur[x] = (line[x] + pr) & 255
        img[y] = cur
        prev = cur
    img = img.reshape(h, w, ch).astype(np.uint8)
    if ctype == 3:
        rgb = plte[img[:, :, 0]]
        a = trns[img[:, :, 0]] if trns is not None and len(trns) else None
        return rgb, (a if a is not None and (a < 255).any() else None)
    if ch == 1:
        return np.repeat(img, 3, 2), None
    if ch == 2:
        return np.repeat(img[:, :, :1], 3, 2), img[:, :, 1]
    if ch == 4:
        a = img[:, :, 3]
        return img[:, :, :3], (a if (a < 255).any() else None)
    return img, None


# ---------------------------------------------------------------- commands


def build(png, out, like=None):
    rgb, alpha = read_png(png)
    h, w = rgb.shape[:2]
    if w & (w - 1) or h & (h - 1):
        raise SystemExit(f"{png}: {w}x{h} is not a power of two")
    lk = None
    container = "zip" if alpha is not None else "pvr"
    if like:
        _r, _a, info = read_texture(like)
        lk = info["hdr"]
        container = info["container"]
        if (lk["width"], lk["height"]) != (w, h):
            print(f"note: size {w}x{h} differs from the original {lk['width']}x{lk['height']}")
    levels = 1 + int(np.log2(max(w, h)))
    rgb_levels = [encode_etc1(m) for m in mip_chain(rgb, levels)]
    pvr = make_pvr(w, h, rgb_levels, lk)
    if container == "pvr":
        if alpha is not None:
            print("note: alpha dropped (the original is a bare PVR without alpha)")
        open(out, "wb").write(pvr)
    else:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
            z.writestr("rgb.pvr", pvr)
            if alpha is not None:
                a3 = np.repeat(alpha[:, :, None], 3, 2)
                z.writestr("alpha.pvr", make_pvr(w, h, [encode_etc1(m) for m in mip_chain(a3, levels)], lk))
        open(out, "wb").write(buf.getvalue())
    print(f"{out}: {w}x{h}, {levels} levels, {container}{' + alpha' if alpha is not None and container == 'zip' else ''}")


def main(a):
    if len(a) < 2:
        sys.exit(__doc__)
    if a[0] == "info":
        rgb, alpha, info = read_texture(a[1])
        hd = info["hdr"]
        print(f"{a[1]}: {info['container']}, {hd['width']}x{hd['height']}, {hd['mips'] + 1} levels, "
              f"format {hd['flags'] & 0xff:#x}, alpha {'yes' if alpha is not None else 'no'}")
    elif a[0] == "topng":
        rgb, alpha, _ = read_texture(a[1])
        write_png(a[2], np.dstack([rgb, alpha]) if alpha is not None else rgb)
        print(f"{a[2]} written")
    elif a[0] == "build":
        like = a[a.index("--like") + 1] if "--like" in a else None
        build(a[1], a[2], like)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
