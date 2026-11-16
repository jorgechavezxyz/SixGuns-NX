#!/usr/bin/env python3
"""Builds the Switch SD card folder for the Six Guns port from the player's own, official game files.

    python3 build_sd.py --apk <APK or XAPK> --obb-dir <folder with the two OBBs> [--out sd] [--stadium]
    python3 build_sd.py --apk ... --obb <main.obb> --obb <patch.obb> ...
    python3 build_sd.py --check --apk ... --obb-dir ...      (only check the files)
    sd_builder_gui.py / the "Six Guns SD Builder" app: the same with a window

Only the official game is accepted (Six Guns 2.9.9a for Android, com.gameloft.android.ANMP.GloftSXHM):
  * the APK must carry Gameloft's own signature, intact (tools/apk_verify.py: certificate, RSA signature,
    every file's digest) and contain the 2.9.9a arm64 engine; repackaged / re-signed / modded copies
    and "installer" APKs are refused;
  * the two OBB data files (main.29431... and patch.29523..., from the phone's
    Android/obb/com.gameloft.android.ANMP.GloftSXHM/ folder) must be byte-identical to the originals.
An XAPK / APKS bundle is accepted when its APK passes the same check (its OBBs are used if present).

Output: <out>/switch/sixguns/ with sixguns.nro, libAndroid.so, base.apk, assets/, obb/, the `lan`
marker (LAN multiplayer; --no-lan to skip) and, with --stadium, mods/30_stadium.zip (needs numpy).
Copy <out>/switch to the root of the SD card. Nothing is downloaded and no game data ships with the port.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import zipfile

FROZEN = getattr(sys, "frozen", False)
BASE = getattr(sys, "_MEIPASS", None) if FROZEN else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(BASE, "tools")
sys.path.insert(0, TOOLS)
import apk_verify  # noqa: E402

KNOWN = json.load(open(os.path.join(TOOLS, "known_files.json"), encoding="utf-8"))
PKG = KNOWN["package"]
LIB = KNOWN["libAndroid_so"]["path_in_apk"]
ENGINE = KNOWN["libAndroid_so"]["sha256"]
OBBS = {KNOWN["main_obb"]["name"]: KNOWN["main_obb"]["sha256"], KNOWN["patch_obb"]["name"]: KNOWN["patch_obb"]["sha256"]}
MAIN_NAME = KNOWN["main_obb"]["name"]


INSTALL_STEPS = """Install Six Guns on your Switch

You need a Switch that runs homebrew (Atmosphère custom firmware).

1. Turn the Switch fully off (hold the power button > Power Options > Turn Off) and take out the
   microSD card. Put it in your computer.

2. Open the SD card. It already has a folder called "switch" (your homebrew lives there).
   Open the output folder of this builder: <OUT>
   Copy the "sixguns" folder INTO the SD card's "switch" folder, so you get:
       SD card/switch/sixguns/sixguns.nro, libAndroid.so, base.apk, assets/, obb/ ...
   Mac: do NOT drag a whole "switch" folder onto the card's "switch" folder; the Finder would offer
   to replace it and delete your other homebrew. Copy "sixguns" into it instead.
   (No "switch" folder on the card? Copy the builder's whole "switch" folder to the card's root.)
   About 850 MB; wait for the copy to finish.

3. Eject the SD card safely (Mac: Eject in the Finder sidebar; Windows: "Safely Remove Hardware"),
   put it back in the Switch and start Atmosphère as usual.

4. Start Six Guns in "title override" mode, which gives homebrew enough memory:
   on the HOME menu, hold R and start any game; the homebrew menu opens instead of the game.
   Release R and pick Six Guns. (The album / applet mode does not have enough memory.)

5. The first start takes a little longer. Your saves are in switch/sixguns/save/ with automatic
   backups in switch/sixguns/save_backups/. Quit with + and - together, or from the HOME menu.

Updating the port later: copy a newer sixguns.nro over switch/sixguns/sixguns.nro (saves stay).
Multiplayer: every console in a match needs the same version; custom maps need the same map mods.
"""


class Refused(Exception):
    """A file is not the official game file; the message says why and what to use instead."""


def sha256_stream(f):
    h = hashlib.sha256()
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
    return h.hexdigest()


def sha256_file(path):
    with open(path, "rb") as f:
        return sha256_stream(f)


def _copy_member(z, name, dest):
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with z.open(name) as src, open(dest, "wb") as out:
        shutil.copyfileobj(src, out, 1 << 20)


def check_apk(path, tmp, log):
    """Verifies the APK (or the APK inside an XAPK/APKS bundle). Returns (path of the verified APK,
    {obb name: (zipfile, member)} found in a bundle)."""
    if not os.path.isfile(path):
        raise Refused(f"APK not found: {path}")
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        raise Refused(f"{os.path.basename(path)} is not an APK (not a zip file; often a download page saved as .apk)")
    names = z.namelist()
    if "assets/app.png" in names and "assets/obb/info.json" in names:
        raise Refused(f"{os.path.basename(path)} is a repackaged 'installer' APK, not Gameloft's. "
                      "Use the official Six Guns 2.9.9a APK and its OBB files.")
    obbs = {}
    if "AndroidManifest.xml" in names:
        apk = path
    else:
        inner = [n for n in names if n.lower().endswith(".apk")]
        if not inner:
            raise Refused(f"{os.path.basename(path)}: not an APK, XAPK or APKS bundle")
        apk = None
        for n in inner:
            dest = os.path.join(tmp, os.path.basename(n))
            _copy_member(z, n, dest)
            try:
                with zipfile.ZipFile(dest) as iz:
                    if LIB in iz.namelist() and "AndroidManifest.xml" in iz.namelist():
                        apk = dest
            except zipfile.BadZipFile:
                pass
        if apk is None:
            raise Refused(f"{os.path.basename(path)}: no APK with the arm64 engine inside the bundle")
        for n in names:
            b = os.path.basename(n)
            if b in OBBS:
                obbs[b] = (z, n)
        log(f"  bundle: {len(inner)} APK(s), {len(obbs)} OBB file(s)")
    try:
        rep = apk_verify.verify_apk(apk, ENGINE)
    except apk_verify.VerifyError as e:
        raise Refused(f"{os.path.basename(path)}: {e}. Use the official, Gameloft-signed Six Guns 2.9.9a APK.")
    log(f"  APK: official Gameloft signature, {rep['files']} files intact, engine 2.9.9a")
    return apk, obbs


def check_obb(name, src, log):
    """src: a file path or (zipfile, member). Refuses anything but the original OBB."""
    if isinstance(src, tuple):
        with src[0].open(src[1]) as f:
            h = sha256_stream(f)
    else:
        h = sha256_file(src)
    if h != OBBS[name]:
        raise Refused(f"{name} is not the original file (it differs from Gameloft's). Copy it again from the phone's "
                      f"Android/obb/{PKG}/ folder.")
    log(f"  {name}: original")


def find_obbs(obb_files, obb_dir, bundle_obbs):
    found = dict(bundle_obbs)
    paths = list(obb_files)
    if obb_dir:
        if not os.path.isdir(obb_dir):
            raise Refused(f"OBB folder not found: {obb_dir}")
        paths += [os.path.join(obb_dir, n) for n in os.listdir(obb_dir)
                  if n.endswith(".obb") and not n.startswith("._")]
    for p in paths:
        b = os.path.basename(p)
        if b.startswith("._"):
            continue  # macOS AppleDouble metadata is not an OBB.
        if b in OBBS:
            found[b] = p
        elif b.endswith(PKG + ".obb"):
            raise Refused(f"{b}: a different game version (Six Guns 2.9.9a uses {', '.join(OBBS)})")
    missing = [n for n in OBBS if n not in found]
    if missing:
        raise Refused("missing " + " and ".join(missing) + f" (on the phone: Android/obb/{PKG}/)")
    return found


def default_nro():
    for p in (os.path.join(BASE, "sixguns.nro"), os.path.join(BASE, "port", "sixguns.nro")):
        if os.path.exists(p):
            return p
    return None


def have_numpy():
    try:
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


def build(apk, obb_files=(), obb_dir=None, out="sd", nro=None, stadium=False, lan=True, check_only=False, log=print):
    """Checks every file first, then (unless check_only) writes the SD folder. Raises Refused on a file
    that is not official. Returns a list of warnings."""
    warnings = []
    tmp = tempfile.mkdtemp(prefix="sixguns-sd-")
    try:
        log("== Checking your game files")
        game_apk, bundle_obbs = check_apk(apk, tmp, log)
        obbs = find_obbs(obb_files, obb_dir, bundle_obbs)
        for name in OBBS:
            check_obb(name, obbs[name], log)
        if check_only:
            log("All files are the official Six Guns 2.9.9a.")
            return warnings
        root = os.path.join(out, "switch", "sixguns")
        os.makedirs(os.path.join(root, "obb"), exist_ok=True)
        log("== Writing " + root)
        with zipfile.ZipFile(game_apk) as z:
            _copy_member(z, LIB, os.path.join(root, "libAndroid.so"))
            n = 0
            for name in z.namelist():
                if name.startswith("assets/") and not name.endswith("/"):
                    _copy_member(z, name, os.path.join(root, name))
                    n += 1
        shutil.copyfile(game_apk, os.path.join(root, "base.apk"))
        log(f"  engine, APK, {n} asset files")
        for name, src in obbs.items():
            dest = os.path.join(root, "obb", name)
            if isinstance(src, tuple):
                _copy_member(src[0], src[1], dest)
            elif os.path.abspath(src) != os.path.abspath(dest):
                shutil.copyfile(src, dest)
        log("  OBB files")
        nro = nro or default_nro()
        if nro and os.path.exists(nro):
            shutil.copyfile(nro, os.path.join(root, "sixguns.nro"))
            log("  sixguns.nro (the port)")
        else:
            warnings.append("no sixguns.nro: download it from the project's releases page and put it in " + root)
        lan_marker = os.path.join(root, "lan")
        if lan:
            open(lan_marker, "w").close()
            log("  LAN multiplayer on")
        elif os.path.exists(lan_marker):
            os.remove(lan_marker)
        if stadium:
            if not have_numpy():
                warnings.append("Stadium map skipped: it needs numpy (python3 -m pip install --user numpy)")
            else:
                log("== Stadium demo map (from your own game files, ~30 s)")
                sys.path.insert(0, os.path.join(TOOLS, "modding", "maps", "stadium"))
                import build_stadium
                work = os.path.join(tmp, "stadium")
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    build_stadium.main(os.path.join(root, "obb", MAIN_NAME), work)
                os.makedirs(os.path.join(root, "mods"), exist_ok=True)
                shutil.copyfile(os.path.join(work, "30_stadium.zip"), os.path.join(root, "mods", "30_stadium.zip"))
                log("  mods/30_stadium.zip (pick 'Stadium' on the multiplayer lobby's map row)")
        total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(root) for f in fs)
        log(f"Done: {root} ({total / 1e6:.0f} MB).")
        log("To install: copy the 'sixguns' folder into the SD card's 'switch' folder, then on the Switch hold R while")
        log("starting any game and pick Six Guns in the homebrew menu (full steps: build_sd.py --install-help).")
        for w in warnings:
            log("WARNING: " + w)
        return warnings
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Six Guns NX: build the SD card from your official game files")
    ap.add_argument("--install-help", action="store_true", help="show how to put the result on the Switch")
    ap.add_argument("--apk", required="--install-help" not in sys.argv, help="the official Six Guns 2.9.9a APK (or an XAPK/APKS holding it)")
    ap.add_argument("--obb", action="append", default=[], help="an OBB file (main and patch)")
    ap.add_argument("--obb-dir", help="folder holding the two OBB files")
    ap.add_argument("--out", default="sd", help="output folder (default: sd)")
    ap.add_argument("--nro", help="the port's sixguns.nro (default: next to this tool / port/sixguns.nro)")
    ap.add_argument("--stadium", action="store_true", help="also generate the Stadium demo map (needs numpy)")
    ap.add_argument("--no-lan", action="store_true", help="do not turn on LAN multiplayer")
    ap.add_argument("--check", action="store_true", help="only check the files, write nothing")
    if "--install-help" in sys.argv:
        print(INSTALL_STEPS.replace("<OUT>", "the output folder (default: sd/switch/sixguns)"))
        return 0
    a = ap.parse_args()
    try:
        build(a.apk, a.obb, a.obb_dir, a.out, a.nro, a.stadium, not a.no_lan, a.check)
    except Refused as e:
        print("REFUSED: " + str(e))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
