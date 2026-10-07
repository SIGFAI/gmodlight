# GModLight: the full plan

Everything still worth doing, ranked, with how to do it. The research behind it
is in [ENGINE_ANALYSIS.md](ENGINE_ANALYSIS.md); RE scripts are in `tools/re/`.

## Where it stands (round 10)

| Area | State |
|---|---|
| Overlay, input, modes, weapon wheel | Works in game |
| Field of view / box alignment | Fixed; verified with `fakehost --fovtest` |
| Turning | Garry's Mod draws where the view will be when shown (prediction) and Dying Light re-projects the rest: world stays exact, the gun only shifts when a turn starts/stops. `fakehost --spin-rate 3`: 0.03� off instead of 3.3� |
| Moving | Garry's Mod draws from where the camera will be (fixes near things, like a grenade at your feet, sliding while walking). DL logs the leftover |
| Physgun on zombies | Works in game; no snap-backs since round 9. Round 10: held zombies extrapolated between GMod updates, GMod interpolation 100 ? 15 ms |
| Kyle's arms and weapon | Found (`PlayerDI+0x338` ? `PlayerFppVis`); round 10 follows its pointers to the real model; untested |
| GMod guns ? DL damage | Kills work. First-hit fault traced to the wound-skin swap running off the game thread; fixed by the message-pump game tick (round 7); untested |
| RPG, grenades, crossbow, bullet impacts | RPG "near perfect" in game; grenades bounce; bullet sparks |
| Weapon look | Lit like the scene, 2560 wide, 4x MSAA, sway/bob/inertia |
| Zombie hitboxes | Fitted to the skeleton; headshots from the head bone |
| Stability | Round 8: no lock shared between Present and game-thread work (freeze risk removed); pointers into a level are dropped when the level changes |

## Next in-game test: what the log answers

1. `threads: Present N, window M` and `world work moved to the game thread`: the
   message-pump tick is running (and whether Present is a different thread).
2. `held actor moved by thread T via ...`: which engine setters the AI uses on a
   held zombie, and from which thread. `snap back? ...`: any jump after release.
3. `own model scan: N game objects walked in X ms, K of the hidden classes`, then
   one line per arm/weapon object with its distance: proves the hiding (and its cost).
4. `TakeDamage ... faulted` should be gone; `damage ... applied` on first hits.
5. `GMod frame age X ms; camera vs where GMod drew from: avg Y cm`: how much
   movement latency is left after prediction.
## 1. Correctness and stability (do first)

### 1.1 First hit faults inside TakeDamage
- Known: engine `IControlObject::TakeDamage` (engine 0x13e310) clones our
  `SDamageInfoDi` through its vtable (slot 1), sets the victim to
  `[[ctrl+8]+0xa0]`, calls the victim's vtable slot 17 with the clone, mirrors
  it into a network event if the object has one, then deletes the clone.
- The fault is on the first hit per actor only, so it's in a first-reaction path
  (alerting, choosing a hit reaction, picking a target from the attacker).
- Plan: read the fault address from the log, disassemble with
  `tools/re/func.py gamedll_x64_rwdi.dll <rva>`, find which `SDamageInfoDi` field
  it dereferences (likely a pointer we leave 0 at 0x40–0xA0, or the attacker),
  fill it the way gamedll 0x6e8680 (the weapon hit) does.
- Until then: `AI::Kill` guarantees deaths, so this only costs hit reactions.

### 1.2 Never run in co-op
- gamedll has `SessionCooperative` / `SessionCooperativeDI` objects and a
  `CScriptConditionCoopNum` condition. Plan: find the session object (RTTI cast
  from the level's objects or the `Evn_SessionEvent` path) and switch GModLight
  off (overlay hidden, no damage/moves) whenever a co-op session is active.
  Today the only guard is the instruction to keep Game Visibility private.

### 1.3 Thread safety, the rest
- Done: world work in the game tick under a mutex. Still in Present: camera
  read (fine, read-only), F8 dumps and the "ray ahead" probe (debug only).
- If the log shows Present and the tick on the same thread, nothing changes;
  if different, watch for any new crash and fall back with `[Advanced] GameThread=0`.

### 1.4 Objects that die or unload while GMod holds them
- Handles are raw pointers. Driven actors are re-found near their last position
  each scan (proves they're alive). A freed-and-reused pointer of another class
  is filtered by `WantedClass`. Remaining hole: an actor freed between the scan
  and the drive in the same ~66 ms. Plan: re-validate with a 1 m `FindNearby`
  right before `SetWorldXform` for anything not seen in the last scan.

## 2. Making it look like one game

### 2.1 Viewmodel shouldn't be re-projected (mostly solved in round 8)
- The re-projection that keeps GMod's world things glued to DL's world while
  turning is applied to the whole frame, so the gun (camera-attached) was swung
  by the 1–2 frames of GMod latency too.
- Round 8: GMod predicts the view ahead by the measured frame age, so only the
  prediction error is re-projected. The gun now shifts only while a turn speeds
  up or slows down. If that's still visible, the full fix below remains.
- Plan: GMod renders two layers: world (boxes, beams, effects) and view
  (viewmodel, hands, HUD, menus). Layer 2 into a render target via
  `render.PushRenderTarget`, composited into the bottom half of a
  double-height capture (the dirty-rect copy keeps it cheap). DL's shader
  re-projects layer 1 only and draws layer 2 as is.

### 2.2 Occlusion: GMod things show through DL walls
- Explosions, beams and debug boxes behind a wall are drawn on top of it.
- Plan A (cheap): for effects GMod knows the position of (explosions, sparks,
  props), use a probe from the camera; hide if DL's world is in between.
- Plan B (proper): capture DL's scene depth (hook `ClearDepthStencilView` /
  `OMSetRenderTargets` to find the main depth target before post-processing,
  copy it), have GMod write its depth (`render.SetWriteDepthToDestAlpha` into an
  A8R8G8B8 target), compare per pixel in the composite shader. Needs DL's depth
  format (likely reversed-Z float) and near/far from the projection matrix.

### 2.3 Thrown zombies and walls, "not perfect"
- The waist-height ray from where the actor is to where GMod wants it misses
  walls hit by the head or feet, and stops only the actor in DL, while GMod's
  box keeps flying until the blocked flag arrives (one scan later).
- Plan: three rays (feet, waist, head) per driven actor per update; send the
  hit normal back; GMod reflects the velocity (bounce off walls, slide on
  floors) instead of zeroing it. Same code as grenade bounces.

### 2.4 Real ragdolls
- Skeleton is readable (319 elements; `bonerag*` are the ragdoll's own bones).
  Plan: on GMod-side death or a big physgun throw, spawn a GMod ragdoll
  (`prop_ragdoll` with a zombie-ish model, invisible), drive DL's bones from it
  with `SetDontApplyAnim` + `SetElementWorldMatrix` (ValveBiped → DL bone map),
  or simply let `AI::Kill` ragdoll and then fling the DL ragdoll's pelvis.

### 2.5 Dying Light's own effects for GMod events
- engine/gamedll have `CFXEmitter`, `FXInstance`, `FXEmitterFireSmoke`,
  `CDecal`/`CDecalMgr`, `FireLightObject`, `CFXLight`, `FxBulletVis`.
- Plan: spawn DL's own bullet impact/decal and explosion FX at probe hits via
  `IGSObject::CreateObject` + their script methods, so impacts leave real DL
  bullet holes and explosions light DL's world. Needs the FX definition names
  (search gamedll strings near `FxBulletVis`).

### 2.6 Lighting, further
- Now: one average colour from the middle of the screen.
- Plan: sample left/right/top regions for a directional light; detect DL's
  flashlight (`FlashlightModule`) and add a spot light to the viewmodel when on;
  flash the viewmodel light on DL muzzle flashes/explosions.

### 2.7 Smaller polish
- Dying Light's own HUD in GMod hands: its weapon quick-slot widget (bottom
  right) and durability icon describe Kyle's hidden weapon. The engine exports
  `IUIElement`/`IUIText` methods; plan: find the HUD root (`Hud*` classes in
  gamedll: `HudFlashlight`, `HudCoopMarker`, ...) and hide the weapon widgets
  while GMod's hands are out, as was done for the arms.
- HUD: GMod's ammo HUD at the bottom right sits where DL's quick slots are;
  move/scale it with a custom HUDPaint (or `hud_*` convars).
- Crosshair: DL's and GMod's can both show; hide DL's in GMod hands
  (find its HUD element) or GMod's.
- Weapon sway tuning convars: `gmodlight_vm_sway`, `gmodlight_viewmodel_fov_scale`,
  `gmodlight_match_lighting`, `gmodlight_light_scale`.

## 3. Features

- **Weapon spawning** (asked for "later"): DL's dev menu has item spawning
  (`LevelDI` methods). GMod's own SWEPs already work through the spawn menu.
- **Props in DL's world**: the probe system now exists; tracking `prop_physics`
  like grenades (bounce, rest on DL ground) gives props that land on streets
  and roofs. Proper collision would need DL's geometry near the player streamed
  into GMod as physics meshes (much bigger).
- **DL explosions from GMod explosions**: `CreateObject(ExplodingObject)` +
  `Explode` to throw DL's physics objects and cars around.
- **Toolgun on DL actors**: e.g. weld/rope between zombies works on the stand-ins
  already; make DL follow both ends.

## 4. Performance

- GMod's read-back is synchronous (`GetRenderTargetData` waits for the GPU).
  Plan: two system-memory surfaces, lock the previous frame's (one frame more
  latency, which re-projection hides for the world layer).
- DL's upload is `UpdateSubresource` of the drawn rectangle on DL's render
  thread. Fine at ~0–50% of 2560x1071; a shared GPU texture would remove it but
  needs D3D9Ex in GMod (it uses plain D3D9).
- GMod at 100 fps (`fps_max 100`); DL at ~165. Raising it costs GPU that DL
  needs (DL was at 92–96% GPU).

## 5. How each change is tested

- `fakehost` stands in for Dying Light: `--real` (real coordinates), `--fovtest`
  (projection check), `--night` (scene light), `--lying` (posed zombie),
  `--width N` (render width), `--spin`, `--physgun-at N`, `--menu-at N`. It
  answers probes with a ground plane and a wall 12 m ahead, and saves frames.
- The GMod self-test fires a rocket and a grenade clear of the test zombie, so
  the physgun (grab → push to 8.5 m → drop) and pistol (7 shots → kill) checks
  still run on a live zombie.
- In game: the plugin log (`Dying Light\GModLight.log`), GMod's
  `garrysmod\gmodlight.log`, and `Dying Light\GModLight_shots\`.
