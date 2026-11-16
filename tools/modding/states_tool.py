"""Six Guns state machines (`states_game` in the OBB): dump to JSON and rebuild.

    python -I states_tool.py dump  <states_game> <folder>
    python -I states_tool.py build <folder> <states_game>

`states_game` holds the behaviour state machines of every character type (NPCs, animals, the
player): states, their parameters, transitions (triggers), animations with timed events.
Format, from StateDatabase::Load -> StateSetData::Load -> StateData::Load -> StateTrack::Load
(all little-endian; string = u16 length + bytes, no terminator; bool = 1 byte):

  u16 set_count
  set:     string name; u16 meta_count; meta[] = (i32, string)   (skipped by the game)
           u16 state_count; state[]
  state:   string name
           u16 param_count;   param[]   = (string key, i32 value)
           u16 trigger_count; trigger[] = (i32 a, i32 b, i32 c, i32 d, i32 e, i32 f, bool has_target,
                                           [string target])   game fields: a+0, b+4, c+8, d+0x18,
                                           e+0xc, f+0x14, target+0x28 of StateTrigger
           u16 anim_count;    anim[]    = (string name, i16 value, u16 track_count, track[])
           u16 animset_count; animset[] = (string name, i16 a, i16 b)
  track:   string name; u16 event_count; event[] = (i32 a, i32 b, i32 c, bool has_text, [string]);
           i32 value

Dump writes `index.json` (set order) and one `<NN>_<set name>.json` per set. Edit freely (add
states, triggers, parameters...); `build` writes the binary back. Field meanings beyond the names
are still being worked out (see tools/readable: StateData, StateTrigger, StateTrack, StateEvent).
"""
import json
import os
import re
import struct
import sys


class R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u16(self):
        v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v

    def i16(self):
        v = struct.unpack_from('<h', self.b, self.p)[0]; self.p += 2; return v

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v

    def b8(self):
        v = self.b[self.p]; self.p += 1; return v

    def s(self):
        n = self.u16(); v = self.b[self.p:self.p + n]; self.p += n
        return v.decode('utf-8', 'surrogateescape')


class W:
    def __init__(self):
        self.out = bytearray()

    def u16(self, v): self.out += struct.pack('<H', v)
    def i16(self, v): self.out += struct.pack('<h', v)
    def i32(self, v): self.out += struct.pack('<i', v)
    def b8(self, v): self.out.append(v)

    def s(self, v):
        e = v.encode('utf-8', 'surrogateescape'); self.u16(len(e)); self.out += e


def read_set(r):
    st = {'name': r.s(), 'meta': [[r.i32(), r.s()] for _ in range(r.u16())], 'states': []}
    for _ in range(r.u16()):
        s = {'name': r.s()}
        s['params'] = [[r.s(), r.i32()] for _ in range(r.u16())]
        trig = []
        for _ in range(r.u16()):
            t = {'v': [r.i32() for _ in range(6)]}
            has = r.b8()
            t['flag'] = has
            if has:
                t['target'] = r.s()
            trig.append(t)
        s['triggers'] = trig
        anims = []
        for _ in range(r.u16()):
            a = {'name': r.s(), 'value': r.i16(), 'tracks': []}
            for _ in range(r.u16()):
                tr = {'name': r.s(), 'events': []}
                for _ in range(r.u16()):
                    ev = {'v': [r.i32(), r.i32(), r.i32()]}
                    has = r.b8()
                    ev['flag'] = has
                    if has:
                        ev['text'] = r.s()
                    tr['events'].append(ev)
                tr['value'] = r.i32()
                a['tracks'].append(tr)
            anims.append(a)
        s['anims'] = anims
        s['animsets'] = [[r.s(), r.i16(), r.i16()] for _ in range(r.u16())]
        st['states'].append(s)
    return st


def write_set(w, st):
    w.s(st['name'])
    w.u16(len(st['meta']))
    for a, b in st['meta']:
        w.i32(a); w.s(b)
    w.u16(len(st['states']))
    for s in st['states']:
        w.s(s['name'])
        w.u16(len(s['params']))
        for k, v in s['params']:
            w.s(k); w.i32(v)
        w.u16(len(s['triggers']))
        for t in s['triggers']:
            for v in t['v']:
                w.i32(v)
            w.b8(t['flag'])
            if t['flag']:
                w.s(t['target'])
        w.u16(len(s['anims']))
        for a in s['anims']:
            w.s(a['name']); w.i16(a['value']); w.u16(len(a['tracks']))
            for tr in a['tracks']:
                w.s(tr['name']); w.u16(len(tr['events']))
                for ev in tr['events']:
                    for v in ev['v']:
                        w.i32(v)
                    w.b8(ev['flag'])
                    if ev['flag']:
                        w.s(ev['text'])
                w.i32(tr['value'])
        w.u16(len(s['animsets']))
        for n, a, b in s['animsets']:
            w.s(n); w.i16(a); w.i16(b)


def dump(src, out):
    r = R(open(src, 'rb').read())
    sets = [read_set(r) for _ in range(r.u16())]
    if r.p != len(r.b):
        print(f'warning: {len(r.b) - r.p} trailing bytes not understood')
    os.makedirs(out, exist_ok=True)
    files = []
    for i, st in enumerate(sets):
        fn = f'{i:02d}_' + re.sub(r'[^A-Za-z0-9_.-]', '_', st['name']) + '.json'
        json.dump(st, open(os.path.join(out, fn), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
        files.append(fn)
    trailing = r.b[r.p:].hex()
    json.dump({'sets': files, 'trailing_hex': trailing}, open(os.path.join(out, 'index.json'), 'w'), indent=1)
    n_states = sum(len(s['states']) for s in sets)
    print(f'{len(sets)} state sets, {n_states} states -> {out}')


def build(src, out):
    idx = json.load(open(os.path.join(src, 'index.json')))
    w = W()
    w.u16(len(idx['sets']))
    for fn in idx['sets']:
        write_set(w, json.load(open(os.path.join(src, fn), encoding='utf-8')))
    w.out += bytes.fromhex(idx.get('trailing_hex', ''))
    open(out, 'wb').write(w.out)
    print(f'built {out}: {len(idx["sets"])} sets, {len(w.out)} bytes')


if __name__ == '__main__':
    cmd, a, b = sys.argv[1:4]
    {'dump': dump, 'build': build}[cmd](a, b)
