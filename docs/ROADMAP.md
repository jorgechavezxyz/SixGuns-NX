# Six Guns port: roadmap

Planned work that is researched but not started. What is done lives in `port/NOTES.md` (session log),
`tools/modding/README.md` (modding formats and tools) and `docs/re/subsystems/` (engine notes).
Last updated 2026-11-15.

---

## 1. Map creator (Blender add-on first, standalone studio later)

Goal: anyone who owns the game can build a multiplayer map, starting from an empty baseplate or an
existing map, and export it as a mod zip that shows up as its own lobby page.

### What already exists (from the Stadium map, `tools/modding/maps/stadium/`)
- Read/write of every format a map needs: levels (`bclara_tool.py`, `clara_format.py`), models
  (`pig_format.py`, `blender_import_pig.py`, `blender_export_pig.py`), textures (`texture_tool.py`),
  collision (`.coll` writer in `build_stadium.py`), minimaps (drawn in the base map's frame).
- Custom maps load as their own lobby page (`port/source/custom_maps.c`, NOTES 29): a mod zip with
  `custom_map.txt` (name, base map, level), files named `<map>__<file>` for per-map replacements.
- Assets come from the user's own APK/OBB at build time; nothing from the game is shipped.
- Known map rules that prevent crashes: keep `Base_Red_Flag` / `Base_Blue_Flag` (Flag::PostInit looks
  them up without a NULL check), drop removed entities from `"g"` groups, replace the LevelDef
  OnStart script, one material name per sub-mesh, unlit material (the lightmapped ones need a baked
  lightmap), collision needs one normal per triangle (masks 3 floor / 8 wall). See NOTES 28-29.

### Phase A: Blender add-on (recommended first; ~3-5 sessions)
Blender (free, Mac + Windows) gives the viewport, gizmos, snapping and undo. A "Six Guns" panel:
1. **Import from APK/OBB**: point at the user's files once; extract a model + texture library.
2. **Asset browser**: the game's ~1,350 models with thumbnails; drag into the scene.
3. **Building blocks**: floor, wall, ramp, stairs, box; team spawn and flag markers.
4. **Templates**: empty baseplate; existing maps (their scenery is baked into a few large models, e.g.
   all Canyon buildings are one `.pig`: they can be kept, deleted or built around, but moving one
   original building needs a model-splitting step).
5. **Collision**: generated from placed objects, per-object "no collision".
6. **Validate + export**: spawns/flags present, size within the base map's 200 m minimap frame,
   name unique; writes the mod zip (level, models, collision, textures, minimap, `custom_map.txt`).
   Optional "test in Ryubing" button.

### Phase B: standalone studio (later; several weeks)
A Roblox-Studio-like app (e.g. Godot) for Mac and Windows reusing the Phase A conversion code: own
renderer for `.pig` + ETC1 textures, editor UI, project files, installers.

### Known limits to lift later
- Lighting is flat (unlit material); real lighting needs lightmap baking or a lit material.
- Every custom map sits on one of the four base maps (sky, sun, 200 m minimap scale).
- No custom scripting/events in maps yet (Lua handlers exist in levels; editing them is possible
  with `bclara_tool.py scripts/build`).
- Sharing: custom maps contain the game's models; fine between owners of the game, keep in mind
  before distributing widely. The tool itself must never ship extracted assets.

---

## 2. Events (bring the EVENTS button back with our own events)

Status: researched, not started. Today the title's EVENTS button opens an empty screen: the event list
came from Gameloft's servers ("Osiris"), which are gone.

### What the game still has (decompile, 2026-11-15)
- `Menu_EventAggregator` lists `LimitedEvent`s (and a clan event); event icons are built-in sprites
  (`s_weeklyEventSmallIconIds` / `MiddleIconIds` / `BigIconIds`); only optional banner pictures were
  downloaded (`.png`).
- All events enter through `LimitedEventsManager::RecieveEventsFromOsiris` (0xc001a4) as JSON; offline
  the manager's `WasLoaded` (+0x20) stays false and `Update` (0xc010b8) does nothing.
- `LimitedEvent::Initialize(Json::Value)` (0xbfbadc) supports, among others:
  - scoring from game actions: `RegisterGameEventListener` / `OnTriggerGameEvent` (the same event bus
    as achievements), `AddScore`, `AddScoreWithCheckForRequiredItems/Weapon` ("use this weapon"
    events), `IsMultiplayerScore`, `IsLotteryScore`, `IsEnergyEvent`, `GetMPGameMode`;
  - goals and rewards: `ParseMinimalGoals`, `CheckForMinGoalReward`, `OsirisAward` / `OsirisGift`
    (keys seen: `reward_coins`, `reward_stars`, `reward_vip`, `gifts`, `awards`, `rank_interval`,
    `item_name0N` / `item_category0N` / `item_amount0N`);
  - timing: start/end dates, `GetDuration`, `IsShortTerm`, `IsEnded`, remaining-time text;
  - presentation: custom title/description (`CustomFields`), icon type, sort priority, tags,
    "go to quest/map" redirect (`GetActionQuest`, `GetActionQuestMap`, `GetEventRedirectType`);
  - leaderboard: `LimitedEventLeaderboard`, `tournament`, `leaderboard_id`,
    `weekly_leaderboard_id`, and a built-in fake leaderboard (`LimitedEventsUtils::
    LimitedEventFakeLeaderboardUserData`) that can be filled with bot names;
  - progress saved in the player's save (`LimitedEventSaveData`).
- No original event data survives: the APK's `assets/events.json` is telemetry definitions only, and
  the save holds no downloaded events.

### Plan (~2-3 sessions)
1. Decode the exact JSON schema from `LimitedEvent::Initialize`, `ParseMinimalGoals`,
   `RegisterGameEventListener` and `LimitedEventsManager::RecieveEventsFromOsiris`.
2. Server clock from the device: hook `ServerTime::IsTimeAvailable` (0x9eadf0) to return true
   (also enables VIP daily bonus and other time-gated features; check each).
3. Port module that feeds authored events once the title loads, rotating weekly by the device
   date, e.g. "Undead Week: kill 200 undead", "Gunslinger: win 5 multiplayer matches",
   "Lucky Draw: lottery score". Events defined in a text/JSON file on the SD card (editable,
   shareable, and loadable from mods).
4. Leaderboards against bots (fake leaderboard data), rewards tuned so the economy is not broken.
5. Fallback if this proves unstable: hide the EVENTS button and its "NEW" badge.

Related local features listed in `docs/re/subsystems/online_services.md` section 4: device time as
server time, local remote config (VIP, prices), daily/gift rewards, "watch video" rewards as free
daily claims.

---

## 3. Other ideas discussed

| Idea | Notes |
|---|---|
| Custom weapon | Easiest as a reskin mod of an existing gun (model, textures, projectile colour, e.g. green energy beams via the game's laser/energy projectile materials); a new shop item needs new item/weapon/projectile data, texts, a shop slot and a save-safe ID. Must be an original design. |
| Bigger custom maps (e.g. a fictional high-school campus) | Needs a larger minimap frame or a different base map; interiors need ceilings, doors and lighting care. |
| PC version | Ryubing wrapper that only runs this game; crossplay over LAN works today (the emulator joins Switch hosts). |
| Android back-port | Same engine library; the port's controller and UI work would need re-hosting on Android. |
| Publishing | Players build from their own APK (2.9.9a); one-step installer, player guide, and a scrub of private data (logs with names/tokens, decompiled code) before anything is shared. |
| Graphics | Graphics menu done (NOTES 30); open: hardware check of High and Compact textures, lighting on custom maps. |
