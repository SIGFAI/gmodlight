# GModLight

Garry's Mod's physgun, weapons and spawn menu inside Dying Light: fling zombies instead of fighting them.

**GModLight is made by [GooseMcGee](https://github.com/GooseMcGee).** All credit for the mod goes to them.

- Original project: https://github.com/GooseMcGee/DyingLight-GMod
- Report bugs and ask questions there: https://github.com/GooseMcGee/DyingLight-GMod/issues
- Upstream release packaged here: [v0.1.0](https://github.com/GooseMcGee/DyingLight-GMod/releases/tag/v0.1.0) (commit [`e3ad71b`](https://github.com/GooseMcGee/DyingLight-GMod/tree/e3ad71bc2e38141321a9804192d7864ec6c96bde))

> **Beta.** Nobody at SIGF has played this build yet. Back up your saves.
> Bugs in the mod itself go to the author's issue tracker above; problems with the one-click install go to this repository's issues.

## What you need

- **Dying Light** ([Steam](https://store.steampowered.com/app/239140/)): current Steam build (no version pinned upstream; engine_x64_rwdi.dll exports and class names resolved at runtime).
- **Garry's Mod** ([Steam](https://store.steampowered.com/app/4000/)): "x86-64 - Chromium + 64-bit binaries" beta branch; started hidden by the Dying Light plugin, never launched by the app.
- ultimate-asi-loader 9.7.4: loads GModLight.asi in Dying Light; installed into the Dying Light folder by the app (https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4).
- gmod-x64: Garry's Mod on the 64-bit beta branch: Steam > Garry's Mod > Properties > Betas > "x86-64 - Chromium + 64-bit binaries" (https://store.steampowered.com/app/4000).
- Windows and the [SIGF app](https://sigf.ai).

## Install

In the SIGF app, open **GModLight** in the catalog, press **Install**, then **Play**. **Restore** puts your game folders back exactly as they were.
The app follows `mashup.json` in this repository: every download is pinned by sha256. The files come from the release [`v0.1.0`](../../releases/tag/v0.1.0).

### How to play

- Dying Light stays the real game; Garry's Mod runs hidden next to it and draws its physgun, weapons and hands into your view.
- Press Play: Dying Light starts, and Garry's Mod starts by itself, hidden, at the main menu. Load into a single player game and press F2.
- F2 Garry's Mod hands on or off. Mouse buttons fire, or grab and freeze with the physgun; the wheel picks weapons or pushes and pulls what you hold.
- F1 or hold Q opens the spawn menu, hold C the context menu, 1-0 weapon slots, R reload or unfreeze, E use or rotate, Z undo.
- Grab zombies with the physgun and throw them into walls, or shoot them with rockets, grenades and the crossbow. F7 hides everything Garry's Mod draws.

### Good to know

- You need Dying Light and Garry's Mod on Steam (Windows), with Garry's Mod on the 64-bit beta branch: Steam > Garry's Mod > Properties > Betas > "x86-64 - Chromium + 64-bit binaries".
- Single player only: set Dying Light's game visibility to private and never join or host co-op with it installed. Do not start Garry's Mod yourself first; let Dying Light start it.
- Restore before playing Garry's Mod online: it removes the GModLight addon and module from Garry's Mod and the ASI loader (xinput1_3.dll) from Dying Light. Keys and options: GModLight.ini in the Dying Light folder.
- Beta (first public version). Report bugs to the authors on the upstream issue tracker with Dying Light\GModLight.log and GarrysMod\garrysmod\gmodlight.log.

## What this repository holds

1. The upstream source tree at tag `v0.1.0`, commit [`e3ad71bc2e38141321a9804192d7864ec6c96bde`](https://github.com/GooseMcGee/DyingLight-GMod/tree/e3ad71bc2e38141321a9804192d7864ec6c96bde), every file unchanged (same git blobs). Upstream's own `README.md` is there, unchanged; GitHub shows this file (`.github/README.md`) first.
2. Added by SIGF in the same commit: this file, and `sigf/` (the scripts that built the release assets, for reference: they run inside the SIGF repository).
3. `mashup.json`, the SIGF app recipe (the next commit).
4. The release `v0.1.0` (its tag is the first commit):

| Asset | Size | sha256 | What it is |
|---|---|---|---|
| `gmodlight-dyinglight.zip` | 595258 B | `0d84070b0a995b92f16f85d011f822679418468e721f6cb0ee40f79c4f78a5c3` | upstream's `GModLight.asi` and `GModLight.ini` from release `v0.1.0` (built by upstream's GitHub Actions from the tag), unchanged; the official Ultimate ASI Loader 9.7.4 x64 (NoPDB) `dinput8.dll`, unchanged, as `xinput1_3.dll` (upstream's layout); under `GModLight/` upstream's LICENSE and THIRD_PARTY_NOTICES and the Ultimate ASI Loader and MinHook licenses; into the Dying Light folder. |
| `gmodlight-gmod.zip` | 220631 B | `ab761f81b525f10620ab71231491d4ec9026aeb35354e61ef9778c5f112e3cfd` | upstream's `gmcl_gmodlight_win64.dll` unchanged, as `garrysmod/lua/bin/gmcl_gmodlight_win64.dll` and `gmsv_gmodlight_win64.dll` (upstream's layout), and the Lua addon `garrysmod/addons/gmodlight/` with upstream's LICENSE; into the Garry's Mod folder. |

The sha256 of every file inside the zips is in `mashup.json` (`contents`).

## Licenses

| Part | License | Where |
|---|---|---|
| GModLight (all of the upstream tree) | MIT, Copyright the GModLight authors | `LICENSE`, `THIRD_PARTY_NOTICES.md` |
| Ultimate ASI Loader 9.7.4 (`xinput1_3.dll`) | MIT, ThirteenAG | https://github.com/ThirteenAG/Ultimate-ASI-Loader |
| MinHook (linked into `GModLight.asi` and the GMod module) | BSD-2-Clause | https://github.com/TsudaKageyu/minhook |

## Why this repository exists

The SIGF app (https://sigf.ai) installs mods from recipes (`mashup.json`) whose downloads are pinned release files. This repository makes GModLight installable in one click, credited to GooseMcGee. If you are the author and want anything changed or taken down, open an issue here.
