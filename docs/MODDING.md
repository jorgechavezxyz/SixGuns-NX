# Modding Six Guns NX

## How mods load

Put mod zips in `switch/sixguns/mods/` on the SD card. Each zip mirrors the OBB layout (`texts`,
`interface`, `textures/...`, `soundpack`, `levels.pak`, ...); any file it contains replaces the
game's copy. The port registers every `mods/*.zip` with the engine's own file system
(`pig::stream::StreamMgr::AddPath`) before the game starts, so mods sit in front of the patch and
main OBBs (`port/source/mods.c`). The log shows `[mods] <name>.zip loaded`.

- Zips must be **stored** (no compression), like the OBBs: the engine memory-maps stored entries and
  some loaders (string tables, UI layouts) only work that way. Use `tools/scripts/pack_mod.py`.
- Several mods: where two contain the same file, the one whose name sorts **last** wins
  (`10_base.zip` < `20_fix.zip`).
- Loose files in `mods/` are ignored (see `mods.c` for why).
- Files in folders (`textures_chars/TopHat_Df.tga`) need a **folder entry** in the zip
  (`textures_chars/`), as the OBBs have: the engine's `ZipFileSystem` builds its folder tree from
  those entries and never finds a file whose folder has none. `pack_mod.py` writes them (since
  2026-11-12; zips made earlier only worked for files at the root, such as `texts` or `levels.pak`).

## Formats and tools

| What | File(s) | Tool | Status |
|---|---|---|---|
| All game text, 15 languages | `texts` | `texts_tool.py dump/build` | done: byte-identical round trip; strings any length; new strings |
| Character behaviour + animation state machines (38 character types: animals, monsters, humans, MP humans, horses) | `states_game` | `states_tool.py dump/build` | done: byte-identical round trip; states/params/transitions/anims editable as JSON; structural edits load |
| Levels: entities, placement, quests, **mission scripts** (Lua event handlers on entities) | `levels.pak` -> `*.bclara` | `bclara_tool.py` (`scripts`/`build`, `dump`/`load`, `pak`) | done (2026-11-12): byte-identical round trip of all 15 levels; every script extracts as readable Lua; edited scripts verified running in Ryubing |
| UI layouts | `interface`, `gui`, `popup`, `mglock` | - | planned |
| Textures (`textures*/*.tga`: PVR v2 ETC1 + mips, bare or rgb/alpha zip) | OBB `textures/`, `textures_chars/`, ... | `texture_tool.py` (`info`, `topng`, `build --like`) | done (2026-11-12): ETC1 codec, re-encode 42 dB PSNR, same header/size as the original; needs numpy (Blender's Python has it) |
| Characters: which model/texture an NPC uses | level data (`NPC` -> `NPCDef` -> `NPCTemplate` -> `.pig` + `Material`) | `bclara_tool.py dump/load` | done: swap an NPCDef's `Template` to give a character another body |
| Models (view/edit in Blender) | `characters/*.pig`, `models/*.pig` | `pig_format.py` (`info`, `obj`), `blender_import_pig.py` (meshes, UVs, textures, armature + weights) | done (2026-11-12): format fully decoded; reader+writer round-trip all 1,352 models byte-identically; `blender_export_pig.py` writes edited Blender meshes back (skin weights included); **a model edited in Blender runs in game** (`examples/blender_model_mod_ingame.png`) |
| Animations, collision | `*.anim`, `*.coll` | - | formats decoded (docs/re/subsystems/engine_core.md §5.3–5.4), no tool yet |
| Sprites / fonts | `*.bsprite` | - | planned |
| Sounds | `soundpack` (XML), `*.mpc`, `*.vxn` | edit XML directly | soundpack readable now |

### Text: example

```
python -I tools/modding/texts_tool.py dump <OBB texts> work/texts
# edit work/texts/en.tsv (id <TAB> name <TAB> text; \n for new lines)
python -I tools/modding/texts_tool.py build work/texts mymod/texts
python -I tools/scripts/pack_mod.py mymod switch/sixguns/mods/20_mytext.zip
```
Take `texts` from the **patch** OBB (it overrides the main one). String ids are
`pack << 11 | index` (`string_ids.h` lists all names); port code can use them with
`StringMgr::GetString` / `LayerButton::SetTextID`. New strings: a row with id `:<pack>` and a new
name; the build assigns the next index and prints the id.

### Character behaviour: example

```
python -I tools/modding/states_tool.py dump <OBB states_game> work/states
# edit work/states/07_NPC_Chicken.json: states, "params", "triggers" (transitions, "target" =
# next state), "anims" (animation file + timed tracks Range/Blend/Loop/events)
python -I tools/modding/states_tool.py build work/states mymod/states_game
python -I tools/scripts/pack_mod.py mymod switch/sixguns/mods/20_mystates.zip
```
`states_game` is in the main OBB. Missions are not in it (they are level data, see .bclara).

### Missions and level scripts: example

Mission logic is Lua 5.1 attached to level entities as event handlers (`OnEnable`, `OnEnter`,
`OnInteract`, `OnKill`, `OnConditionSuccess`, `OnTimer`, `OnDestroy`, ...): 11,051 handlers across the
15 levels. Every one is already extracted, readable, in `tools/game_data/scripts/<level>/` (folder =
level folder/quest, file = `Entity__OnEvent.lua`). The functions they call are documented in
`docs/re/LUA_API.md` (263 engine bindings + the engine's Lua prelude helpers).

```
python3 -I tools/modding/bclara_tool.py scripts tools/game_data/levels/a04.bclara work/a04
# edit work/a04/.../SomeEntity__OnInteract.lua (plain Lua)
python3 -I tools/modding/bclara_tool.py build tools/game_data/levels/a04.bclara work/a04 work/a04.bclara
python3 -I tools/modding/bclara_tool.py pak tools/game_data/raw/main/levels.pak mymod/levels.pak a04.bclara=work/a04.bclara
python3 -I tools/scripts/pack_mod.py mymod switch/sixguns/mods/20_mymission.zip
```
- Each shipped handler is compiled Lua 5.1 bytecode whose chunk name is the full source, so the
  extracted `.lua` is the exact original. `build` puts an edited handler back as **plain source**
  (the engine's `luaL_loadbuffer` compiles it at load); unchanged handlers keep their bytecode, and a
  build with no edits is byte-identical.
- A handler file usually defines one function (`function X266486_G() ... end`) that the engine calls
  by name; keep the function name and put your code inside it.
- `Print("...")` is empty in this release; with the empty marker file `lua_print` in
  `switch/sixguns/` the port logs every call as `[lua] ...` (debug aid for script mods; leave it off
  for normal play, the shipped scripts print a lot).
- Syntax errors make the engine reject that handler at load; check edited files first (any Lua 5.1
  checker, e.g. `luac5.1 -p file.lua`).
- Whole-level edits (entity positions, params, adding entities) go through JSON:
  `bclara_tool.py dump <level> level.json` / `load level.json <out.bclara>` (large: the main world is
  ~58,000 entities). Strings that are new are appended to the level's string table automatically.
- Verified 2026-11-12 in Ryubing: a04 with an extra `Print` in `A04/LevelDef__OnStart.lua`, packed
  as `mods/20_luatest.zip` (whole `levels.pak`), logged `[lua] MODTEST: edited A04 OnStart script ran`.


### Level placement: example (verified in Ryubing 2026-11-12)

Moving, adding or retuning entities goes through the JSON dump (every entity: name, template, `pos`,
`rot` quaternion, `scale`, typed params):

```bash
python3 -I tools/modding/bclara_tool.py dump  a04.bclara a04.json     # edit a04.json
python3 -I tools/modding/bclara_tool.py load  a04.json   a04_new.bclara
python3 -I tools/modding/bclara_tool.py build a04_new.bclara scripts/ a04_final.bclara   # optional: script edits too
python3 -I tools/modding/bclara_tool.py pak   game_data/raw/main/levels.pak mod/levels.pak a04.bclara=a04_final.bclara
python3 -I tools/scripts/pack_mod.py mod 20_mymod.zip                  # -> switch/sixguns/mods/
```

Test that was run: `Chest_001` moved by (+40, +30, 0) in `a04` and `LevelDef` `OnStart` given
`Print("... " .. tostring(GetDistance("Chest_001", "Chest_002")))`; with the `lua_print` marker the
game logged 354.997 (the moved distance; the original placement gives 313.590), no errors. Position
units are metres; z is up.

### Textures and characters: example

```bash
PY=/Applications/Blender.app/Contents/Resources/5.2/python/bin/python3.13    # or any python3 with numpy
$PY -I tools/modding/texture_tool.py topng TopHat_Df.tga TopHat_Df.png        # paint it in any editor
$PY -I tools/modding/texture_tool.py build TopHat_Df_new.png mod/textures_chars/TopHat_Df.tga --like TopHat_Df.tga
python3 -I tools/scripts/pack_mod.py mod 20_mymod.zip
```

Get the original from the OBB (`unzip -p main.*.obb textures_chars/TopHat_Df.tga > TopHat_Df.tga`).
A texture is shared by every model that names it, so a change shows on all of them. Finding what a
character uses: the `NPC` entity's `NPCDef` param -> that NPCDef's `Template` -> the NPCTemplate's
`model` (`characters/<name>.pig`) and `Materials`; the `.pig` names its textures (`*_Df` colour,
`*_Mk` mask, `*_Nm` normal map). Example chain for the tutorial bandit `Q_NPC_Tutos3_Fight_Foe_001`:
NPCDef 181181 `NPCD_Tutorial_TopHat_Brawler` -> NPCTemplate 141626 `NPCT_OutlawTopHatA` ->
`characters/outlawtophat.pig` -> `textures_chars/TopHat_Df.tga` (512x512, both head variants, hat,
vest, shirt, trousers on one atlas). Giving a character another body: set the NPCDef's `Template` to
another human NPCTemplate (e.g. 147169 `NPCT_Sheriff`) in the level JSON (`dump`/`load`).

### Models in Blender

```bash
unzip -p <main OBB> characters/outlawtophat.pig > outlawtophat.pig
unzip -j <main OBB> 'textures_chars/TopHat_*' -d tex/          # the textures the model names
/Applications/Blender.app/Contents/MacOS/Blender --python tools/modding/blender_import_pig.py -- outlawtophat.pig --textures tex
```

One object per body part of the most detailed LOD (`--lod 2/3` for the others), the game texture as
material, and an armature (`<model>_rig`) with vertex groups, so the character can be posed. Characters
contain their variants as separate objects (e.g. two bodies and four heads for the top-hat outlaw):
hide the ones you don't want. `python3 -I tools/modding/pig_format.py obj model.pig out.obj` gives a
plain OBJ for other tools. Facts the importer relies on (checked on the data): node rotations use the
transpose of the usual quaternion convention; world = parent * local; skin offsets are stored unscaled and
blended as `sum weight * (bone * offset)`. Back into the game:

```bash
Blender --background edited.blend --python tools/modding/blender_export_pig.py -- outlawtophat.pig mod/characters/outlawtophat.pig
python3 -I tools/scripts/pack_mod.py mod 20_mymodel.zip
```

The original `.pig` is the template (skeleton, materials, LOD groups); every importer-named object
(`<model>_<mesh>_<sub>`) replaces its sub-mesh: UV seams split, normals/tangents recomputed, skin records
rebuilt from the vertex groups (max 4 bones per vertex). The importer loads one LOD at a time (`--lod`);
edit each LOD you want changed (the game picks by distance) or the far LODs keep the old shape. Keep
objects and rig at the origin and edit the mesh itself. New characters can reuse a model's skeleton
and animations by keeping its bone names. Checked: widening the top-hat outlaw (all LODs) shows in the
game with animation intact.

### Animations in Blender

```bash
python3 -I tools/modding/anim_format.py check <main OBB>        # 2539/2544 exact (the other 5 are 0-byte files)
python3 -I tools/modding/anim_format.py info anims/human_duel_drawb.anim
unzip -j <main OBB> 'anims/human_duel_*' -d anims/
/Applications/Blender.app/Contents/MacOS/Blender --python tools/modding/blender_import_anim.py -- \
    outlawtophat.pig --textures tex anims/human_duel_idle.anim anims/human_duel_drawb.anim \
    --play human_duel_drawb [--render out --frames 0,0.5,1] [--save out.blend]
```

The script imports the model with `blender_import_pig.py`, then adds one action per `.anim` (fake user, named
after the file; the scene fps is set to the clip's fps and keys sit at `ms * fps / 1000`). Switch clips with
the rig's Action selector. Bones are matched to animation nodes by name; helper nodes the model lacks
(`Master`, `Bip01_Footsteps`) are skipped, so there is no root motion. The game's quaternions use the same
(transposed) convention as the `.pig` nodes; the importer converts them to each bone's own rest frame
(`basis = C^-1 * L0^-1 * L * C`, `C` = constant node-to-bone rotation from `bone.matrix_local`), so the
arbitrary bone roll does not matter. Hold keys become constant interpolation. To use it in a Blender
session already holding a model from `blender_import_pig.py`, call
`import_anim(rig, pig_format.parse(open(pig,'rb').read()), anim_path)`. Exporting animations back to `.anim`
is not written.

## Custom multiplayer maps

A mod zip that contains a `custom_map.txt` adds a map to the multiplayer lobby (its own page after the
four built-in maps; "any map" never picks it):

```
name=Stadium
base=2
level=custommaps/stadium/stadium.bclara
```

- `base`: the built-in map it sits on (0 El Matadero, 1 Bloodwine, 2 Canyon, 3 Necropolis). The custom map
  uses the base map's sky, sun and minimap frame (LevelWidth 200 m across the minimap) and every game check
  that a level is a multiplayer map sees the base map. The base map itself is not changed.
- `level`: the map's level file inside the zip. Zip entries are found by **file name** (folders are
  ignored by the lookup), so give it a unique name.
- `<name>__<file>` (lower case), e.g. `stadium__a12_tdm_forest_minimap.tga`: replaces `<file>` only during
  this map's matches (here the minimap image).
- Models and collision referenced by the level should have their own names (`stadium.pig`,
  `stadium.coll`) so nothing of the base map is overridden.
- Every console in a match needs the same map mods: maps are numbered 5, 6, ... in name order, and a
  console without the map does not see such rooms (the server browser marks them "not installed").

Things a map must get right (each one crashed or broke a test map):
- keep the flag base entities `Base_Red_Flag` / `Base_Blue_Flag` (the game looks them up by name);
- remove deleted entities from `"g"` groups and replace scripts that reference removed groups;
- collision (`.coll`): one unit normal per triangle (the 4th index of each triangle), floor triangles
  mask 3, walls mask 8; without normals the spawn probe finds no ground and players start at the
  world origin;
- one material name per model sub-mesh (sub-meshes sharing a name share one texture), and an unlit
  material unless you also supply a baked lightmap.

### Example: the Stadium

`tools/modding/maps/stadium/build_stadium.py <main.obb> <outdir>` (Python 3 + numpy) builds
`<outdir>/30_stadium.zip` from your own OBB: field, 400 m track, bleachers, goal posts, a stadium wall,
team spawns and flags in the end zones, its own minimap. `tools/build_sd.py --stadium` runs it for you.
Read it as a worked example of every format above.
