"""Reader/writer for Six Guns "clara" level files (.bclara), shared by bclara_tool.py.

Format from the engine's loaders (tools/readable/engine/clara.cpp): ClaraFile::Load,
LoadClaraFile, Type::Skip, Template::LoadV100, Node::LoadV100, Folder::LoadV100, Group::Load,
Entity::Load, Param::Load / LoadKF, Movie::LoadV100, Clip::LoadV100, PSTemplate::Load (particle
systems). All little-endian. Strings in the file are indices into the file's string table ("TT
strings", ClaraFile::GetTTString); the reader resolves them to text and the writer rebuilds the
table, so names can be edited as text.

Every reader returns plain dicts/lists (JSON-friendly) and the writer consumes the same, so
dump -> JSON -> build round-trips. Unknown/opaque parts are kept as hex.
"""
import struct


class R:
    def __init__(self, b, strings=None):
        self.b, self.p, self.strings = b, 0, strings or []

    def u8(self): v = self.b[self.p]; self.p += 1; return v
    def u16(self): v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v
    def u32(self): v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v
    def f32(self): v = struct.unpack_from('<f', self.b, self.p)[0]; self.p += 4; return v
    def raw(self, n): v = self.b[self.p:self.p + n]; self.p += n; return v
    def vec(self, n): return [self.f32() for _ in range(n)]

    def pstr(self):  # pig string: u16 length + bytes
        n = self.u16(); return self.raw(n).decode('utf-8', 'surrogateescape')

    def tt(self):  # string-table index -> text (None if out of range)
        i = self.u32()
        return self.strings[i] if i < len(self.strings) else {'idx': i}


class W:
    def __init__(self):
        self.out = bytearray()
        self.strings, self.index = [], {}

    def u8(self, v): self.out.append(v & 0xff)
    def u16(self, v): self.out += struct.pack('<H', v)
    def u32(self, v): self.out += struct.pack('<I', v & 0xffffffff)
    def f32(self, v): self.out += struct.pack('<f', v)
    def raw(self, b): self.out += b
    def vec(self, v): [self.f32(x) for x in v]

    def pstr(self, s):
        e = s.encode('utf-8', 'surrogateescape'); self.u16(len(e)); self.out += e

    def tt(self, s):
        if isinstance(s, dict):
            self.u32(s['idx']); return
        if s not in self.index:
            raise KeyError(f'string not in table: {s!r}')
        self.u32(self.index[s])


# ---------------------------------------------------------------- params

def read_value(r, ptype):
    v = {'comp': r.tt()}
    if ptype == 0:
        v['f'] = r.f32()
    elif ptype == 1:
        v['u'] = r.u32(); v['s'] = r.tt()
    elif ptype == 2:
        v['u'] = r.u32()
    elif ptype == 3:
        n = r.u32(); v['text'] = r.raw(n).decode('utf-8', 'surrogateescape')
    elif ptype == 4:
        has = r.u8(); v['has'] = has
        if has:
            tag = r.u8(); v['tag'] = tag
            if tag == ord('e'):
                v['b'] = r.u8(); v['template'] = r.u32(); v['entity'] = read_entity(r)
    elif ptype == 5:
        v['s'] = r.tt(); v['s2'] = r.tt()
    elif ptype == 6:
        n = r.u8(); v['data'] = r.raw(n * 4).hex()
    elif ptype == 7:
        v['a'] = r.u32(); v['b'] = r.u32()
    return v


def write_value(w, ptype, v):
    w.tt(v['comp'])
    if ptype == 0:
        w.f32(v['f'])
    elif ptype == 1:
        w.u32(v['u']); w.tt(v['s'])
    elif ptype == 2:
        w.u32(v['u'])
    elif ptype == 3:
        e = v['text'].encode('utf-8', 'surrogateescape'); w.u32(len(e)); w.raw(e)
    elif ptype == 4:
        w.u8(v['has'])
        if v['has']:
            w.u8(v['tag'])
            if v['tag'] == ord('e'):
                w.u8(v['b']); w.u32(v['template']); write_entity(w, v['entity'])
    elif ptype == 5:
        w.tt(v['s']); w.tt(v['s2'])
    elif ptype == 6:
        d = bytes.fromhex(v['data']); w.u8(len(d) // 4); w.raw(d)
    elif ptype == 7:
        w.u32(v['a']); w.u32(v['b'])


def read_values(r):
    code = r.u16(); n = r.u16()
    ptype = code.bit_length() - 2  # ilog2(code) - 1
    return code, ptype, [read_value(r, ptype) for _ in range(n)]


def read_param(r):
    p = {'name': r.tt(), 'anim': r.u8()}
    p['code'], ptype, p['values'] = read_values(r)
    return p


def write_param(w, p):
    w.tt(p['name']); w.u8(p['anim']); write_kf(w, p)


def write_kf(w, p):
    ptype = p['code'].bit_length() - 2
    w.u16(p['code']); w.u16(len(p['values']))
    for v in p['values']:
        write_value(w, ptype, v)


# ---------------------------------------------------------------- entity

def read_entity(r):
    e = {'id': r.u32(), 'name': r.tt(), 'pos': r.vec(3), 'rot': r.vec(4), 'scale': r.vec(3)}
    clip = r.u32()
    e['clip'] = clip
    if clip:
        e['clip_a'] = r.u32(); e['clip_b'] = r.u32()
    att = r.u32()
    e['attached'] = att
    if att:
        e['attached_a'] = r.u32()
    params = []
    for _ in range(r.u16()):
        tag = r.u8()
        params.append(read_param(r) if tag == ord('p') else {'tag': tag})
    e['params'] = params
    return e


def write_entity(w, e):
    w.u32(e['id']); w.tt(e['name']); w.vec(e['pos']); w.vec(e['rot']); w.vec(e['scale'])
    w.u32(e['clip'])
    if e['clip']:
        w.u32(e['clip_a']); w.u32(e['clip_b'])
    w.u32(e['attached'])
    if e['attached']:
        w.u32(e['attached_a'])
    w.u16(len(e['params']))
    for p in e['params']:
        if 'tag' in p:
            w.u8(p['tag'])
        else:
            w.u8(ord('p')); write_param(w, p)


# ---------------------------------------------------------------- nodes

def read_node(r):
    return {'id': r.u32(), 'name': r.tt()}


def write_node(w, n):
    w.u32(n['id']); w.tt(n['name'])


def read_group(r):
    return {'a': r.u32(), 'name': r.tt(), 'members': [r.u32() for _ in range(r.u32())]}


def write_group(w, g):
    w.u32(g['a']); w.tt(g['name']); w.u32(len(g['members']))
    for m in g['members']:
        w.u32(m)


def read_movie(r):
    m = read_node(r)
    m['clips'] = [[r.u32(), r.u32()] for _ in range(r.u32())]
    return m


def write_movie(w, m):
    write_node(w, m); w.u32(len(m['clips']))
    for a, b in m['clips']:
        w.u32(a); w.u32(b)


def read_clip(r):
    c = read_node(r)
    c['fps'] = r.u32(); c['frames'] = r.u32()
    c['pos'] = [[r.u32(), r.vec(3), r.u32()] for _ in range(r.u32())]
    c['rot'] = [[r.u32(), r.vec(4), r.u32()] for _ in range(r.u32())]
    c['scale'] = [[r.u32(), r.vec(3)] for _ in range(r.u32())]
    tracks = []
    for _ in range(r.u32()):
        t = {'path': r.tt(), 'keys': []}
        for _ in range(r.u32()):
            k = {'t': r.u32(), 'interp': r.u32()}
            k['code'], _pt, k['values'] = read_values(r)
            t['keys'].append(k)
        tracks.append(t)
    c['param_tracks'] = tracks
    return c


def write_clip(w, c):
    write_node(w, c); w.u32(c['fps']); w.u32(c['frames'])
    w.u32(len(c['pos']))
    for t, v, i in c['pos']:
        w.u32(t); w.vec(v); w.u32(i)
    w.u32(len(c['rot']))
    for t, v, i in c['rot']:
        w.u32(t); w.vec(v); w.u32(i)
    w.u32(len(c['scale']))
    for t, v in c['scale']:
        w.u32(t); w.vec(v)
    w.u32(len(c['param_tracks']))
    for tr in c['param_tracks']:
        w.tt(tr['path']); w.u32(len(tr['keys']))
        for k in tr['keys']:
            w.u32(k['t']); w.u32(k['interp']); write_kf(w, k)


def read_folder(r, pstemplate_reader):
    f = read_node(r)
    n = r.u16()
    items = []
    i = 0
    while i < n:
        tag = r.u8()
        if tag == ord('e'):
            items.append({'e': {'b': r.u8(), 'template': r.u32(), **read_entity(r)}})
        elif tag == ord('f'):
            items.append({'f': read_folder(r, pstemplate_reader)})
        elif tag == ord('g'):
            items.append({'g': read_group(r)})
        elif tag == ord('i'):
            items.append({'i': read_clip(r)})
        elif tag == ord('m'):
            m = read_movie(r)
            n += len(m['clips'])  # Movie::GetNumClips: its clips follow in this folder
            items.append({'m': m})
        elif tag == ord('s'):
            items.append({'s': pstemplate_reader(r)})
        else:
            items.append({'tag': tag})
        i += 1
    f['items'] = items
    return f


def write_folder(w, f, pstemplate_writer):
    write_node(w, f)
    n = sum(1 for it in f['items']) - sum(len(it['m']['clips']) for it in f['items'] if 'm' in it)
    w.u16(n)
    for it in f['items']:
        if 'e' in it:
            e = it['e']; w.u8(ord('e')); w.u8(e['b']); w.u32(e['template']); write_entity(w, e)
        elif 'f' in it:
            w.u8(ord('f')); write_folder(w, it['f'], pstemplate_writer)
        elif 'g' in it:
            w.u8(ord('g')); write_group(w, it['g'])
        elif 'i' in it:
            w.u8(ord('i')); write_clip(w, it['i'])
        elif 'm' in it:
            w.u8(ord('m')); write_movie(w, it['m'])
        elif 's' in it:
            w.u8(ord('s')); pstemplate_writer(w, it['s'])
        else:
            w.u8(it['tag'])


def collect_strings(obj, out):
    """All text values that came from the string table (for rebuilding it)."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k in ('name', 'comp', 's', 's2', 'path') and isinstance(v, str):
                out.add(v)
            else:
                collect_strings(v, out)
    elif isinstance(obj, list):
        for x in obj:
            collect_strings(x, out)


# ---------------------------------------------------------------- particle systems (PSTemplate)

def read_kf(r):
    code, _pt, values = read_values(r)
    return {'code': code, 'values': values}


def read_shape(r):
    # PSTemplate::LoadShape: duration, emit interval min/max, looped, position (vec3), particle count,
    # speed, emit surface; then LoadSphere/DiskShape: radius, angle, rotation (quaternion).
    s = {'duration': r.u32(), 'interval': [r.u32(), r.u32()], 'looped': r.u8(), 'pos': r.vec(3),
         'count': r.u32(), 'speed': r.f32(), 'surface': r.u32()}
    s['radius'], s['angle'] = r.f32(), r.f32()
    s['rot'] = r.vec(4)
    return s


def write_shape(w, s):
    w.u32(s['duration']); w.u32(s['interval'][0]); w.u32(s['interval'][1]); w.u8(s['looped']); w.vec(s['pos'])
    w.u32(s['count']); w.f32(s['speed']); w.u32(s['surface'])
    w.f32(s['radius']); w.f32(s['angle']); w.vec(s['rot'])


INFLUENCES = {  # tag -> field layout (f = f32, u = u32, q = 4 floats, v = 3 floats)
    'b': 'ffuu', 'f': 'qf', 'g': 'f', 'm': 'vff', 'w': 'qf'}


def read_infl(r, layout):
    out = []
    for c in layout:
        out.append(r.f32() if c == 'f' else r.u32() if c == 'u' else r.vec(4) if c == 'q' else r.vec(3))
    return out


def write_infl(w, layout, vals):
    for c, v in zip(layout, vals):
        if c == 'f': w.f32(v)
        elif c == 'u': w.u32(v)
        else: w.vec(v)


def read_particle_def(r):
    d = {'rot': [r.f32(), r.f32()], 'random_rot_dir': r.u8(), 'alignment': r.u32(), 'align_speed': r.f32()}
    d['frames'] = [{'color': r.u32(), 'damping': r.f32(), 'size': r.vec(2), 'center': r.vec(2),
                    'rot_speed': r.f32(), 'tex0': r.vec(2), 'tex1': r.vec(2)} for _ in range(r.u32())]
    d['material'] = read_kf(r)
    d['layers'] = [[read_kf(r) for _ in range(4)] for _ in range(r.u16())]
    d['tail'] = [r.f32(), r.f32(), r.f32(), r.u8()]
    return d


def write_particle_def(w, d):
    w.f32(d['rot'][0]); w.f32(d['rot'][1]); w.u8(d['random_rot_dir']); w.u32(d['alignment']); w.f32(d['align_speed'])
    w.u32(len(d['frames']))
    for f in d['frames']:
        w.u32(f['color']); w.f32(f['damping']); w.vec(f['size']); w.vec(f['center'])
        w.f32(f['rot_speed']); w.vec(f['tex0']); w.vec(f['tex1'])
    write_kf(w, d['material'])
    w.u16(len(d['layers']))
    for layer in d['layers']:
        for kf in layer:
            write_kf(w, kf)
    t = d['tail']; w.f32(t[0]); w.f32(t[1]); w.f32(t[2]); w.u8(t[3])


def read_pstemplate(r):
    ps = {'id': r.u32(), 'name': r.tt(), 'emitters': []}
    for _ in range(r.u16()):
        em = {'def': read_particle_def(r)}
        tag = r.u8()
        em['shape_tag'] = tag
        if tag in (ord('s'), ord('d')):
            em['shape'] = read_shape(r)
        infl = []
        for _ in range(r.u32()):
            t = chr(r.u8())
            infl.append({'type': t, 'v': read_infl(r, INFLUENCES[t]) if t in INFLUENCES else []})
        em['influences'] = infl
        ps['emitters'].append(em)
    return ps


def write_pstemplate(w, ps):
    w.u32(ps['id']); w.tt(ps['name']); w.u16(len(ps['emitters']))
    for em in ps['emitters']:
        write_particle_def(w, em['def'])
        w.u8(em['shape_tag'])
        if em['shape_tag'] in (ord('s'), ord('d')):
            write_shape(w, em['shape'])
        w.u32(len(em['influences']))
        for inf in em['influences']:
            w.u8(ord(inf['type']))
            if inf['type'] in INFLUENCES:
                write_infl(w, INFLUENCES[inf['type']], inf['v'])


# ---------------------------------------------------------------- whole file

def read_file(b):
    r = R(b)
    lvl = {'version': r.u32()}
    n = r.u32()
    lvl['strings'] = r.strings = [r.pstr() for _ in range(n)]
    types = []
    for _ in range(r.u16()):
        tag = r.u8()
        types.append({'tag': tag, 'name': r.tt(), 'fields': [r.tt() for _ in range(r.u16())]} if tag == ord('y') else {'tag': tag})
    lvl['types'] = types
    tpls = []
    for _ in range(r.u16()):
        tag = r.u8()
        tpls.append({'tag': tag, **read_node(r), 'flag': r.u8()} if tag == ord('t') else {'tag': tag})
    lvl['templates'] = tpls
    files = []
    nf = r.u16()
    lvl['file_count_field'] = nf
    for _ in range(nf + 1):
        f = {'a': r.u32(), 'name': r.tt()}
        tag = r.u8()
        f['tag'] = tag
        if tag == ord('g'):
            f['group'] = read_group(r)
        elif tag == ord('f'):
            f['folder'] = read_folder(r, read_pstemplate)
        files.append(f)
    lvl['files'] = files
    lvl['trailing_hex'] = b[r.p:].hex()
    return lvl


def write_file(lvl):
    w = W()
    w.strings = list(lvl['strings'])
    used = set()
    collect_strings({k: v for k, v in lvl.items() if k != 'strings'}, used)
    for s in sorted(used - set(w.strings)):  # new names added while editing go at the end
        w.strings.append(s)
    w.index = {}
    for i, s in enumerate(w.strings):
        w.index.setdefault(s, i)
    w.u32(lvl['version']); w.u32(len(w.strings))
    for s in w.strings:
        w.pstr(s)
    w.u16(len(lvl['types']))
    for t in lvl['types']:
        w.u8(t['tag'])
        if t['tag'] == ord('y'):
            w.tt(t['name']); w.u16(len(t['fields']))
            for f in t['fields']:
                w.tt(f)
    w.u16(len(lvl['templates']))
    for t in lvl['templates']:
        w.u8(t['tag'])
        if t['tag'] == ord('t'):
            write_node(w, t); w.u8(t['flag'])
    w.u16(lvl['file_count_field'])
    for f in lvl['files']:
        w.u32(f['a']); w.tt(f['name']); w.u8(f['tag'])
        if f['tag'] == ord('g'):
            write_group(w, f['group'])
        elif f['tag'] == ord('f'):
            write_folder(w, f['folder'], write_pstemplate)
    w.raw(bytes.fromhex(lvl['trailing_hex']))
    return bytes(w.out)
