# Mad Max – Convoy Respawn Mod

Makes destroyed convoys in Mad Max (2015, PC) come back — in the same play
session, driven by the game's own logic, without touching the wreck, the
hood ornament or the region Threat.

**Status: public beta (v0.9.0-beta1).** Only the non-Steam (GOG) build of
`AVAMain.exe` dated 2015-12-07 is supported; the mod verifies the executable
at startup and disables itself on anything else.

➡ **Players: grab the zip from the [Releases](../../releases) page.** The
README inside explains installation, how it works, the F3/F4 keys and how to
report problems.

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
  [gigaHours/mm_sdk](https://github.com/gigaHours/mm_sdk)).
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
