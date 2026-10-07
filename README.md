# GModLight

**Garry's Mod's physgun, weapons and spawn menu inside Dying Light.**

Dying Light stays the real game: its world, zombies, parkour and combat. Garry's
Mod runs hidden next to it, and its hands, physgun, weapons and spawn menu are
drawn into Dying Light's view. Grab zombies with the physgun and fling them into
walls, shoot them with Garry's Mod weapons, fire rockets into Dying Light's streets.

## Download and play

1. Get the latest **GModLight-x.y.z.zip** from the [Releases](https://github.com/GooseMcGee/DyingLight-GMod/releases/latest) page.
2. You need **Dying Light** and **Garry's Mod** on Steam, with Garry's Mod on the
   64-bit branch: *Garry's Mod → Properties → Betas → "x86-64 - Chromium + 64-bit binaries"*.
3. Close both games, unzip, and double-click **Install.bat**. It finds both games
   through Steam.
4. Start Dying Light from Steam. Garry's Mod starts by itself (hidden) at Dying
   Light's main menu. Load in and press **F2**.

Updating: run the new version's Install.bat (your `GModLight.ini` is kept).
Uninstall: **Uninstall.bat**.

**Single player only.** Set Dying Light's game visibility to private and don't
join or host co-op with this installed.

### Controls

| Key | What it does |
|---|---|
| F2 | Garry's Mod hands on/off (physgun and weapons replace Kyle's) |
| F1 / hold Q | Spawn menu |
| hold C | Context menu |
| Mouse buttons | Fire; physgun grab / freeze |
| Mouse wheel | Weapon wheel; push/pull what the physgun holds |
| 1-0 | Weapon slots |
| R / E / Z | Reload (unfreeze) / use (rotate with the physgun) / undo |
| F7 | Hide / show everything Garry's Mod draws |
| F9 | Show the zombie stand-in boxes (debug) |

Keys and options are in `GModLight.ini` in the Dying Light folder (frame cap,
what to hide, damage scale, ...).

### Problems?

[Open an issue](https://github.com/GooseMcGee/DyingLight-GMod/issues) and attach `Dying Light\GModLight.log` and
`GarrysMod\garrysmod\gmodlight.log`. Common fixes are in the README.txt inside
the download.

## How it works

```
Dying Light (host)                         Garry's Mod (hidden)
 GModLight.asi                              gmcl/gmsv_gmodlight_win64.dll + addons/gmodlight
   camera (+ timing)       --shared mem-->    view, player eye, physgun aim (predicted ahead)
   keys/mouse in GMod hands/menus -->         spawn menu (vgui) / weapons via real commands
   nearby zombies + skeletons -->             invisible stand-ins the physgun and guns hit
   moves/damages/kills zombies <--            stand-ins GMod drives, damage events
   traces through DL's world  <-->            rockets, grenades, bullets meet DL's walls
   overlay  <--  GMod frame (magenta keyed, distances in alpha, one per DL frame)
```

- Dying Light loads `GModLight.asi` through Ultimate ASI Loader (as `xinput1_3.dll`)
  and starts Garry's Mod on an empty `gm_flatgrass` with the world switched off.
- Garry's Mod clears everything but its own things to magenta; Dying Light draws
  that frame over its own, re-projected to its current camera using each
  object's distance, so GMod's things stay put on Dying Light's world.
- The two run in lockstep at Dying Light's frame rate (60 by default).
- Zombies are mirrored as stand-ins fitted to their skeletons. Physgunned ones
  move the real zombie (Dying Light's position writers are hooked so its AI
  can't pull it back); damage goes through the zombie's own `TakeDamage`, and
  deaths through its `Kill`.

The full engine research is in [docs/ENGINE_ANALYSIS.md](docs/ENGINE_ANALYSIS.md);
what's next is in [docs/PLAN.md](docs/PLAN.md).

## Development

Needs Visual Studio 2022 (C++ desktop workload) and CMake 3.20+.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File install.ps1   # installs your build into both games
```

`install.ps1 -Uninstall` removes it. `tools\package.ps1` makes the download zip
(`out\GModLight-<version>.zip`).

### Testing without Dying Light

`build\Release\fakehost.exe` plays Dying Light's side of the bridge and drives
Garry's Mod through scripted tests, printing results and saving frames:

```powershell
build\Release\fakehost.exe 36 --physgun-at 9 --real     # physgun, pistol, rocket, grenade
build\Release\fakehost.exe 26 --spin-rate 3             # turning: prediction and re-projection checks
build\Release\fakehost.exe 19 --parallax-move 0.1       # depth-aware re-projection vs GMod's own render
build\Release\fakehost.exe 20 --fovtest                 # projection matches the camera
```

### Publishing an update

Every push to `main` is built by GitHub Actions. To publish a release:

```powershell
powershell -ExecutionPolicy Bypass -File tools\release.ps1 0.1.1 "What changed" "Another change"
```

That sets `VERSION`, adds the lines to `CHANGELOG.md`, commits, tags `v0.1.1`
and pushes; GitHub Actions builds the zip and creates the release with those
notes.

### Layout

- `shared/bridge.h`: the shared-memory layout both sides use (`kVersion` must match).
- `dl_plugin/`: the Dying Light plugin (`GModLight.asi`).
- `gmod_module/`: Garry's Mod binary module: bridge access from Lua, frame capture.
- `gmod_addon/gmodlight/`: Garry's Mod Lua: view, input, weapons, stand-ins, effects.
- `tools/fakehost.cpp`: the test harness; `tools/re/`: reverse-engineering scripts.
- `dist/`: the default `GModLight.ini` and the download's README.txt.
- `third_party/`: MinHook, Facepunch gmod-module-base, Ultimate ASI Loader
  (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## License

MIT, see [LICENSE](LICENSE). Unofficial fan project, not affiliated with Techland
or Facepunch. Contains no game files.
