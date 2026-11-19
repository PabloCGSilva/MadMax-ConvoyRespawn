# Mad Max – Convoy Respawn Mod

Makes destroyed convoys in Mad Max (2015, PC) come back — in the same play
session, driven by the game's own logic, without touching the wreck, the
hood ornament or the region Threat.

**Status: public beta (v0.9.3-beta1, Enhanced Convoys + World Repop).** Made and tested on the GOG build of
`AVAMain.exe` (2015-12-07). The game functions are found by byte signature,
and since beta3 the mod waits for Steam's DRM to decrypt the executable before
looking, so the Steam version should work too (not confirmed yet). If anything
can't be found, the mod disables itself and says so.

➡ **Players: grab the zip from the [Releases](../../releases) page.** The
README inside explains installation, how it works and how to
report problems.

## World Repop (v0.9.3-beta1)

The same plugin also puts enemies back into a cleared Wasteland (GOG build only; it switches itself off on any other executable):

* **Camps** (31: scrap, fuel and food depots and the camps that guard a vehicle): every 20 to 40 minutes of game time the enemy takes back
  a liberated camp away from the player (3 at most). A game message names it, its map icon shows as enemy territory, the enemies are back
  when you arrive and a counter shows how many are left. Kill them all and the camp is retaken; the civilians return once you leave, and it
  is safe for 30 to 60 minutes. The Top Dogs of Great Watcher and Large Blade come back as one more enemy; lookouts of a pulled-down tower
  come back on the ground. No structure, drummer or item comes back, and saves always store the camp as liberated.
* **Scavenging locations** (135), **sniper towers** (35, rebuilt) and **minefields** (30, mines buried again) come back on the same timer.
* **Road encounters** keep their rate when a region's threat is zero.

The code is in `camp_repop.inc` (with `camp_notice.inc` and the `*_table.inc` files) and `threat_diag.inc`. `MM_WORLD_REPOP` compiles it
without the dev tools. Test override: `scripts\repop_test.txt` with `min max` cooldown seconds, then the camp codes to attack first.

## How it works (short version)

Vanilla marks a convoy as wrecked forever (`CConvoyDataContainer::m_Flags`
bit 0, saved). Its choreographer graph script runs once, handles the wreck
(map icon, relic, despawn, threat transfer), exits, and the object drops out
of the update queue.

This mod, once a second, resolves the 14 convoy containers and their
LogicGraph objects by ID. A wrecked convoy whose graph object has reached its
"done" state (hood ornament collected, wrecks despawned) gets a random
180–480 s timer; when it elapses and the player is > 2000 m from the wreck,
the mod clears the wrecked flag, sets the graph object's state back to 0 and
calls `CGameObject::AddToUpdate()` — the engine then rebuilds the graph and
its processor and fires the default Start node, exactly like a save load
does, scoped to that one object.

The reverse-engineering trail (graph-script wiring format, variable-pin hash
resolution, `UpdatePostSim` state machine, `CTurnTaker`, etc.) is documented
in the research project this was built in; ask if you want it.

## Building

* Visual Studio 2022 Build Tools with the C++ workload (MSVC v143), NuGet
  (`boost` package restored from `packages.config`).
* Clone with submodules: `git clone --recursive ...` (`mm/` is
  [PabloCGSilva/mm_sdk](https://github.com/PabloCGSilva/mm_sdk), a fork of
  [gigaHours/mm_sdk](https://github.com/gigaHours/mm_sdk) with a guarded
  GOG build probe that `plugin.cpp` uses).
* `MSBuild mm_plugin.sln /p:Configuration=Release /p:Platform=x64`
  produces `mm_ezstorm.asi` (rename as you like; any `.asi` in the game's
  `scripts` folder is loaded by the ASI loader).
* `plugin.cpp` has a `MM_DEV_TOOLS` switch: `0` = distributable build,
  `1` = adds the reverse-engineering hotkeys/hooks used during development.

## Credits and licensing

* Started from [gigaHours/MadMaxEzStorm](https://github.com/gigaHours/MadMaxEzStorm)
  and its `mm_sdk` (MinHook + Dear ImGui + game structures). Neither
  upstream repository states a license; they are used here as published,
  with attribution, and this repo makes no claim over that code.
* Everything added in `plugin.cpp` for the convoy respawn is released under
  the MIT license (see `LICENSE-plugin.md`).
* Ultimate ASI Loader by ThirteenAG (MIT) is redistributed in the release zip.
