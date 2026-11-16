# PyInstaller spec for the "Six Guns SD Builder" app (macOS .app, Windows folder with .exe).
#   python -m pip install pyinstaller numpy
#   python -m PyInstaller --noconfirm tools/packaging/sd_builder.spec      (from the repository root)
# Output: dist/Six Guns SD Builder.app (macOS) or dist/Six Guns SD Builder/Six Guns SD Builder.exe (Windows).
# Build the port first (port/sixguns.nro) so the app carries it. Use a Python with Tk 8.6 (python.org or
# actions/setup-python): macOS's own /usr/bin/python3 has Tk 8.5, whose windows hang on current macOS.
import os
import sys

root = os.path.abspath(os.path.join(SPECPATH, "..", ".."))
tools = os.path.join(root, "tools")
datas = [
    (os.path.join(tools, "known_files.json"), "tools"),
    (os.path.join(tools, "apk_verify.py"), "tools"),
    (os.path.join(tools, "build_sd.py"), "tools"),
    (os.path.join(tools, "modding"), os.path.join("tools", "modding")),
    (os.path.join(tools, "scripts", "pack_mod.py"), os.path.join("tools", "scripts")),
]
nro = os.path.join(root, "port", "sixguns.nro")
if os.path.exists(nro):
    datas.append((nro, "."))
else:
    print("WARNING: port/sixguns.nro not built; the app will ask players for it")

a = Analysis(
    [os.path.join(tools, "sd_builder_gui.py")],
    pathex=[tools],
    datas=datas,
    # build_stadium and its helpers are imported at run time from the bundled tools/ folder; numpy and the
    # standard modules they use must be collected explicitly
    hiddenimports=["numpy", "json", "struct", "copy", "math", "io", "contextlib", "zipfile", "base64", "re"],
    excludes=["matplotlib", "scipy", "pandas", "PIL"],
)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="Six Guns SD Builder", console=False)
coll = COLLECT(exe, a.binaries, a.datas, name="Six Guns SD Builder")
if sys.platform == "darwin":
    app = BUNDLE(coll, name="Six Guns SD Builder.app", bundle_identifier="io.github.sixgunsnx.sdbuilder",
                 info_plist={"NSHighResolutionCapable": True})
