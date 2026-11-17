# PyInstaller spec for "sixguns-sd-build", the command-line SD Builder (same code as the app, no window).
#   python -m pip install pyinstaller numpy
#   python -m PyInstaller --noconfirm tools/packaging/sd_builder_cli.spec      (from the repository root)
# Output: dist/sixguns-sd-build/sixguns-sd-build[.exe]. The Six Guns Remaster-NX desktop launcher bundles
# this folder and installs the game with it, so the PC/Mac version gets exactly the files (and the same
# sixguns.nro) a Switch gets from the SD Builder. Self-check: sixguns-sd-build --self-test
import os

root = os.path.abspath(os.path.join(SPECPATH, "..", ".."))
tools = os.path.join(root, "tools")
datas = [
    (os.path.join(tools, "known_files.json"), "tools"),
    (os.path.join(tools, "apk_verify.py"), "tools"),
    (os.path.join(tools, "modding"), os.path.join("tools", "modding")),
    (os.path.join(tools, "scripts", "pack_mod.py"), os.path.join("tools", "scripts")),
    (os.path.join(tools, "packaging", "sixguns.nro"), "."),
]

a = Analysis(
    [os.path.join(tools, "build_sd.py")],
    pathex=[tools],
    datas=datas,
    # build_stadium and its helpers are imported at run time from the bundled tools/ folder
    hiddenimports=["numpy", "json", "struct", "copy", "math", "io", "contextlib", "zipfile", "base64", "re"],
    excludes=["matplotlib", "scipy", "pandas", "PIL", "tkinter"],
)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="sixguns-sd-build", console=True)
coll = COLLECT(exe, a.binaries, a.datas, name="sixguns-sd-build")
