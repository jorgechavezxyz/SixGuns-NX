#!/usr/bin/env python3
"""Six Guns level tool (.bclara): mission scripts and level data, editable.

  python3 -I bclara_tool.py info    <level.bclara>
  python3 -I bclara_tool.py scripts <level.bclara> <outdir>          extract every Lua handler
  python3 -I bclara_tool.py build   <level.bclara> <scriptsdir> <out.bclara>
  python3 -I bclara_tool.py dump    <level.bclara> <out.json>        whole level as JSON
  python3 -I bclara_tool.py load    <in.json> <out.bclara>
  python3 -I bclara_tool.py pak     <levels.pak> <out levels.pak> <a04.bclara=path> [...]

Where the scripts are: entities carry event params (OnEnable, OnEnter, OnInteract, OnKill,
OnConditionSuccess, OnTimer, OnDestroy, ...) whose values are Lua 5.1 chunks. In the shipped files
each chunk is compiled bytecode ("\\x1bLuaQ", int 4 / size_t 4 / instruction 4 / number 8) whose
chunk name is the complete source text, so `scripts` recovers the exact source. The engine loads a
chunk with luaL_loadbuffer (LuaScript::LoadFromBuffer), which also accepts plain source (the Lua
parser is compiled in), so `build` writes an edited script back as source text; unchanged scripts
keep their original bytecode, and a build without edits is byte-identical to the input.

Getting a modified level into the game: the engine reads levels from `levels.pak` (a zip inside the
main OBB; Game::Init -> StreamMgr::AddZipStreamFactory("levels.pak")). Rebuild it with `pak` and
put it in a mod folder at `levels.pak`, then `tools/scripts/pack_mod.py` -> switch/sixguns/mods/.

`build` syntax-checks every edited script first (luac5.1, luac or luajit, whichever is installed;
`brew install luajit` on a Mac) and writes nothing if one fails.

Format: clara_format.py (byte-identical round trip of all 15 shipped levels, 2026-11-12).
"""
import hashlib
import json
import shutil
import subprocess
import os
import re
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clara_format as cf  # noqa: E402

LUA_SIG = "\x1bLua"


# ---------------------------------------------------------------- Lua chunks

def chunk_source(text):
    """(kind, source) of a script value: kind 'bytecode' (source = embedded chunk name) or 'source'."""
    b = text.encode("utf-8", "surrogateescape")
    if not b.startswith(b"\x1bLua"):
        return "source", b
    if len(b) < 12 or b[4] != 0x51:
        return "bytecode-unknown", b""
    sizet = b[8]
    if sizet == 4:
        n = struct.unpack_from("<I", b, 12)[0]
        off = 16
    else:
        n = struct.unpack_from("<Q", b, 12)[0]
        off = 20
    src = b[off:off + n]
    if src.endswith(b"\x00"):
        src = src[:-1]
    return "bytecode", src


def is_script(param_name, values):
    return any(isinstance(v, dict) and isinstance(v.get("text"), str) and
               (v["text"].startswith(LUA_SIG) or re.match(r"^On[A-Z]", param_name or "")) for v in values)


# ---------------------------------------------------------------- walking

def walk_entities(lvl):
    """Yield (path, folder_path, entity) for every entity, including entities embedded in params."""
    def from_entity(e, path, fpath):
        yield path, fpath, e
        for pi, p in enumerate(e.get("params", [])):
            for vi, v in enumerate(p.get("values", []) if isinstance(p, dict) else []):
                if isinstance(v, dict) and isinstance(v.get("entity"), dict):
                    yield from from_entity(v["entity"], path + ["params", pi, "values", vi, "entity"],
                                           fpath + [f"{e.get('name')}.{p.get('name')}"])

    def from_folder(f, path, fpath):
        for i, it in enumerate(f.get("items", [])):
            if "e" in it:
                yield from from_entity(it["e"], path + ["items", i, "e"], fpath)
            elif "f" in it:
                yield from from_folder(it["f"], path + ["items", i, "f"], fpath + [str(it["f"].get("name"))])

    for fi, fl in enumerate(lvl["files"]):
        if "folder" in fl:
            yield from from_folder(fl["folder"], ["files", fi, "folder"], [str(fl.get("name"))])


def get(lvl, path):
    o = lvl
    for k in path:
        o = o[k]
    return o


def safe(s):
    s = str(s) if s is not None else "_"
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", s)[:80] or "_"


def scripts_of(lvl):
    """Yield (path_to_value, folder_path, entity, param_name, value_index, value)."""
    for path, fpath, e in walk_entities(lvl):
        for pi, p in enumerate(e.get("params", [])):
            if not isinstance(p, dict) or "values" not in p:
                continue
            if not is_script(p.get("name"), p["values"]):
                continue
            for vi, v in enumerate(p["values"]):
                if isinstance(v, dict) and isinstance(v.get("text"), str) and v["text"]:
                    yield path + ["params", pi, "values", vi], fpath, e, p.get("name"), vi, v


# ---------------------------------------------------------------- commands

def read_level(path):
    return cf.read_file(open(path, "rb").read())


def cmd_info(level):
    lvl = read_level(level)
    ents = list(walk_entities(lvl))
    scr = list(scripts_of(lvl))
    events = {}
    for _p, _f, _e, name, _vi, _v in scr:
        events[name] = events.get(name, 0) + 1
    print(f"{level}: version {lvl['version']:#x}, {len(lvl['strings'])} strings, {len(lvl['templates'])} templates")
    print(f"  top-level files: {', '.join(str(f.get('name')) for f in lvl['files'])}")
    print(f"  entities: {len(ents)}   scripts: {len(scr)}")
    for k, v in sorted(events.items(), key=lambda kv: -kv[1]):
        print(f"    {k}: {v}")


def cmd_scripts(level, outdir):
    lvl = read_level(level)
    os.makedirs(outdir, exist_ok=True)
    rows, used = [], set()
    for vpath, fpath, e, event, vi, v in scripts_of(lvl):
        kind, src = chunk_source(v["text"])
        base = os.path.join(*[safe(x) for x in fpath], f"{safe(e.get('name'))}__{safe(event)}")
        if vi:
            base += f"__{vi}"
        rel = base + ".lua"
        k = 2
        while rel in used:
            rel = f"{base}__id{e.get('id')}_{k}.lua"
            k += 1
        used.add(rel)
        full = os.path.join(outdir, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        open(full, "wb").write(src)
        rows.append((rel, json.dumps(vpath), e.get("id"), e.get("name"), event, kind, hashlib.sha1(src).hexdigest()))
    with open(os.path.join(outdir, "index.tsv"), "w") as f:
        f.write("file\tpath\tentity_id\tentity\tevent\tkind\tsha1\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    with open(os.path.join(outdir, "README.txt"), "w") as f:
        f.write(f"Lua scripts of {os.path.basename(level)} ({len(rows)} handlers).\n"
                "Edit any .lua file, then:\n"
                f"  python3 -I bclara_tool.py build {os.path.basename(level)} <this folder> <out.bclara>\n"
                "Each file is one event handler of one entity (folder/entity__Event.lua). The functions\n"
                "they call are the engine's Lua bindings: docs/re/LUA_API.md.\n")
    print(f"{len(rows)} scripts -> {outdir} (index.tsv)")


def syntax_error(path):
    """Lua 5.1 syntax check with luac5.1/luac/luajit if one is installed; None if fine or no checker."""
    for tool, args in (("luac5.1", ["-p"]), ("luac", ["-p"]), ("luajit", ["-b"])):
        exe = shutil.which(tool)
        if not exe:
            continue
        cmd = [exe] + args + [path] + (["/dev/null"] if tool == "luajit" else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
        return (r.stderr or r.stdout).strip() if r.returncode else None
    return None


def cmd_build(level, sdir, out):
    lvl = read_level(level)
    changed = 0
    errors = []
    for line in list(open(os.path.join(sdir, "index.tsv")))[1:]:
        rel, _path, _id, _name, _ev, _kind, sha = line.rstrip("\n").split("\t")
        full = os.path.join(sdir, rel)
        if hashlib.sha1(open(full, "rb").read()).hexdigest() != sha:
            err = syntax_error(full)
            if err:
                errors.append(f"{rel}: {err}")
    if errors:
        sys.exit("Lua syntax errors, nothing written:\n  " + "\n  ".join(errors))
    for line in list(open(os.path.join(sdir, "index.tsv")))[1:]:
        rel, path, _id, _name, _ev, kind, sha = line.rstrip("\n").split("\t")
        new = open(os.path.join(sdir, rel), "rb").read()
        if hashlib.sha1(new).hexdigest() == sha:
            continue
        v = get(lvl, json.loads(path))
        v["text"] = new.decode("utf-8", "surrogateescape")
        changed += 1
        print(f"  changed: {rel}")
    data = cf.write_file(lvl)
    open(out, "wb").write(data)
    print(f"{out}: {changed} script(s) replaced as source, {len(data)} bytes")


def cmd_dump(level, out):
    lvl = read_level(level)
    json.dump(lvl, open(out, "w", encoding="utf-8", errors="surrogateescape"), ensure_ascii=True)
    print(f"{out} written")


def cmd_load(inp, out):
    lvl = json.load(open(inp, encoding="utf-8", errors="surrogateescape"))
    data = cf.write_file(lvl)
    open(out, "wb").write(data)
    print(f"{out}: {len(data)} bytes")


def cmd_pak(pak_in, pak_out, repl):
    rep = dict(x.split("=", 1) for x in repl)
    with zipfile.ZipFile(pak_in) as zi, zipfile.ZipFile(pak_out, "w") as zo:
        for info in zi.infolist():
            data = open(rep.pop(info.filename), "rb").read() if info.filename in rep else zi.read(info)
            ni = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            ni.compress_type = info.compress_type
            ni.external_attr = info.external_attr
            zo.writestr(ni, data)
    if rep:
        sys.exit(f"not in {pak_in}: {', '.join(rep)}")
    print(f"{pak_out} written")


def main(a):
    if len(a) < 2:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == "info":
        cmd_info(a[1])
    elif cmd == "scripts":
        cmd_scripts(a[1], a[2])
    elif cmd == "build":
        cmd_build(a[1], a[2], a[3])
    elif cmd == "dump":
        cmd_dump(a[1], a[2])
    elif cmd == "load":
        cmd_load(a[1], a[2])
    elif cmd == "pak":
        cmd_pak(a[1], a[2], a[3:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
