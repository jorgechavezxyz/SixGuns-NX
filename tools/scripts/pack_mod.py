"""Pack a mod folder into a zip the port loads from switch/sixguns/mods/.

    python -I pack_mod.py <mod folder> <out.zip>

The folder mirrors the OBB layout: `texts`, `interface`, `textures/foo.tga`,
`soundpack`, `levels.pak`, ... Every file is stored uncompressed (ZIP_STORED),
like the OBBs: the engine memory-maps stored entries, and some loaders
(string tables, UI layouts) only work on mapped streams.
Name zips so the one that should win sorts last: 10_base.zip, 20_fix.zip.
"""
import os
import sys
import zipfile


def main():
    pack(sys.argv[1], sys.argv[2])


def pack(src, out):
    """Stored zip of the folder src (importable: the SD builder app calls it in-process)."""
    n = 0
    with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_STORED) as z:
        # Like the OBBs, each folder gets its own entry ("textures_chars/") before
        # its files: the engine's ZipFileSystem builds its folder tree from these
        # entries, and a file in a folder without one is never found (seen
        # 2026-11-12: a texture in textures_chars/ was ignored until this).
        for root, dirs, files in os.walk(src):
            dirs.sort()
            rel = os.path.relpath(root, src).replace(os.sep, '/')
            if rel != '.':
                z.writestr(zipfile.ZipInfo(rel + '/'), b'')
            for f in sorted(files):
                p = os.path.join(root, f)
                arc = os.path.relpath(p, src).replace(os.sep, '/')
                z.write(p, arc)
                n += 1
    print(f'{out}: {n} file(s), stored')


if __name__ == '__main__':
    main()
