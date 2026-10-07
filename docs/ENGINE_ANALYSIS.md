# Dying Light × Garry's Mod: engine analysis

Everything learned about Dying Light's engine (Chrome Engine 6) while building GModLight, what
the current build does with it, what the next test will settle, and a ranked plan for the rest.

Build analysed: Steam, `DyingLightGame.exe` 1.55.0.0 (build id 24076292). Addresses are RVAs in
`engine_x64_rwdi.dll` (11 MB, 6,817 named exports) and `gamedll_x64_rwdi.dll` (31 MB, 7 exports,
but full MSVC RTTI). The scripts that found all of this are in `tools/re/`.

---

## 1. Status at a glance

| Area | State | Evidence |
|---|---|---|
| Loading, overlay, input, GMod hands, weapon wheel | Works in game | user tests |
| Camera alignment | Fixed (forward vector is inverted in DL) | scene reports: zombies "180° behind" |
| Physgun grabs zombies | Works in game; jitter fixed in current build (untested) | user test + drive-check log |
| GMod damage → DL zombie | Reaches DL's damage handler; current build sends weapon-style damage | "applied" log lines |
| Guaranteed kills | `AI::Kill` script call when GMod's health runs out (current build) | fakehost |
| Own arms/weapon hidden | Works | screenshot |
| Thrown zombies stop at DL walls | Current build, depends on `Raytrace` (untested) | — |
| GMod things hidden behind DL walls | Not done; design in §7.3 | — |
| Real ragdolls | Not done; design in §7.1 | — |

---

## 2. How the engine is put together

### 2.1 Modules
- `engine_x64_rwdi.dll`: the Chrome Engine. Exports most of its C++ classes by mangled name
  (`IControlObject`, `IModelObject`, `ILevel`, `IBaseCamera`, `IGSObject`, physics `IPh*`, UI).
  Calling these is ordinary x64: `this` in `rcx`; class types returned by value come back
  through a hidden pointer passed right after `this`.
- `gamedll_x64_rwdi.dll`: Dying Light's game code (AI, weapons, damage, player). Almost no
  exports, but MSVC RTTI is intact, so every class's name, vtables and base classes can be
  recovered (`tools/re/layout.py`, `vtof.py`).
- `rd3d11_x64_rwdi.dll`: the D3D11 renderer; exports only `CreateRenderer`.

### 2.2 Getting at the game
- No exported global for `IGame`/`ILevel`. GModLight hooks `IGame::GetActiveLevel` and
  `ILevel::GetActiveCamera` and keeps the pointers the game passes.
- `ILevel::FindObjectsInRadius(vector*, center, radius, CRTTI* filter, bool, int*)` lists the
  `IControlObject*`s near a point. `ttl::vector` is read as either {begin, end} or {data, size}.
- Class of any object: MSVC RTTI from `vtable[-1]` (`eng::ClassName`), and casts between base
  classes by name (`eng::CastTo`, using the ClassHierarchyDescriptor offsets).

### 2.3 Object layout (actors)
`HumanAI` (every humanoid AI: zombies and human NPCs) and `PlayerDI` share one layout:

| Offset | Bases |
|---|---|
| `+0x00` | `IModelObject`, `ModelObject`, `ObjectDI`, `ActorDI` (`AI`, `HumanAI` / `PlayerDI`) |
| `+0x18` | `IControlObject`, `ControlObject` (this is what `FindObjectsInRadius` returns) |
| `+0x28` | `IGSObject`, `GameObject`, `CRTTIObject`, `IObject` |

So the zombie *is* its model: skeleton functions work on `obj - 0x18` directly. (An earlier guess
that the body was a separate `...AIVis` child was wrong for zombies; the player has a
`WeaponVis` child for the held weapon.)

### 2.4 Coordinates
- Meters, Y up, left-handed. GModLight maps to Source as `(x, z, y) × 52.49` relative to an
  anchor, which keeps Source inside its ±16384-unit world.
- **`IBaseCamera::GetForwardVector` returns the camera's +Z axis; the camera looks down −Z.**
  GModLight negates it. Until this was found, GMod's camera faced backwards: boxes moved with the
  view, bullets went behind the player. With it negated, `left == forward × up` (left-handed).
- Field of view: taken from `IBaseCamera::GetProjectionMatrix` (`m[1][1] = 1/tan(vfov/2)`),
  not from `GetFOV`, whose meaning isn't documented.
- An actor's `GetWorldXform` translation is its feet. The camera sits ~1.6 m above them.

---

## 3. Damage

### 3.1 The call
`IControlObject::TakeDamage(const SDamageInfo&)` is virtual, vtable slot 20 (`+0xa0`). Game code
calls exactly this on the target's `IControlObject` (`target + 0x18`); `HumanAI` doesn't override
it. The engine implementation (`engine+0x13e310`):
1. returns early if the target's flags (`[[this+8]+0x40]`, bits 32/33) say so;
2. clones the info (`info->vtbl[1]`), sets `victim` (`+0x10`), converts type 8 to 7;
3. hands it to the game's handler: `[[this+8]+0xa0]->vtbl[0x88/8](info)` (skipped on network
   clients);
4. queues it for replication; deletes the clone.

### 3.2 `SDamageInfoDi` (gamedll vftable `+0x1443f38`, 0xA8 bytes)
Recovered from the copy constructor (`gamedll+0x278e30`) and two builders:
the explosion-on-physics-object code (`+0x2772d1`) and a weapon hit (`+0x6e8680`).

| Offset | Field | Weapon hit | Explosion |
|---|---|---|---|
| 0x00 | vftable | SDamageInfoDi | SDamageInfoDi |
| 0x08 | attacker | owner `+ 0x28` (its `IGSObject`) | damager `+ 0x28` |
| 0x10 | victim | 0 (filled by TakeDamage) | 0 |
| 0x18 | amount | from weapon (`vtbl+0x990`) | computed |
| 0x1c | impulse | `max(dist × 30, 135)` | 0 |
| 0x20 | hit position | `GetElementWorldPos(bone)` | blast centre |
| 0x2c | direction | from caller | from blast |
| 0x38 | mesh element hit | `GetMeshElemFromBoneID(target, 2)` | -1 |
| 0x3c | `EDamageType` | from weapon (`vtbl+0x988`) | 7 |
| 0x40 | flag | 1 | 1 |
| 0x48 | id | weapon/item id | a gamedll global (`+0x1BA818C`) |
| 0x50 | float | weapon stat (`vtbl+0x9e0`) | 0 |
| 0x54 | flags | 0x10 | 0x8000000 |
| 0x70..0x7c | ints | -1, 2, -1, 0 | -1, 0, -1, 0 |
| 0x80 | int | -1 | -1 |
| 0x84, 0x88 | multipliers | 1.0, 1.0 | 1.0, 1.0 |
| 0x9c, 0xa0 | floats | 1.0, -1.0 | 1.0, -1.0 |

### 3.3 `EDamageType` values
From gamedll's enum registration (`tools/re/enumvals.py`): CUT 1, BULLET 2, BLAST 3,
BLAST_SPECIAL 4, ELECTRIC 5, HEAT 6, COLLISION_FROM_GAME_SCRIPT 7, DROWNING 9, ROPE 10, FIRE 11,
PUNCH1–3 12–14, ALL 16, POISON 28, SUICIDER_BLAST 29, IMPACT 30, BRAIN_BLAST 37, FLARE 42,
UNKNOWN -1. The first version sent 7 (copied from the explosion code); zombies ignored it.

### 3.4 What GModLight sends now
GMod damage type → `EDamageType`: bullets BULLET, `DMG_CLUB` PUNCH2, `DMG_SLASH` CUT, `DMG_BLAST`
BLAST, fire FIRE, shock ELECTRIC, crush/fall/physics IMPACT. Weapon-hit layout for everything but
blasts, with the **nearest skeleton bone** to the hit as `0x38`/position (so headshots land on the
head), impulse `135 + amount`, and the **player's `IGSObject` as attacker**. Amount is GMod's ×5.

### 3.5 The fallback that always works
GMod keeps its own health per actor (`gmodlight_actor_health`, 35; headshots, the top 18% of the
box, ×2.5). When it runs out, DL calls the actor's script method `TakeDamageToKill` or `Kill`
(§5). So GMod weapons kill even if DL's damage handler still rejects the hand-built info.

### 3.6 What the next test settles
`GModLight.log` will show, per hit: type, bone, attacker, and "applied". If zombies stagger and
die from damage alone, §3.2 is right. If they only die at the kill call, the handler is rejecting
the info; next suspects, in order: `0x48` (item id) and `0x50` (weapon stat) need real values;
the attacker must be a weapon-owning actor; `0x54` flags.

---

## 4. Movement and physics

### 4.1 Moving actors
`IControlObject::SetWorldXform` moves an actor, but its AI moves it back every frame; the two
fought and the zombie jittered (drive-check log: 1–8 m off within 150 ms). The current build hooks
`SetWorldXform`, `SetWorldXformNoPropagate` and `SetLocalXform` and, while GMod drives an actor,
replaces whatever position the engine tries to give it ("pins"). If the AI moves bodies through
some other path, the next suspects are `IModelObject::SetDontApplyAnim` (stop animation) and the
AI's character controller (not yet found).

### 4.2 Raycasts
`IGSObject::Raytrace(flags, SCollision*, from, to&, ushort, ignore, 0, 0, 0)` (`engine+0x255ba0`)
goes through `[[this+0x20]+0x48]` (the level's physics world) and moves `to` to the hit point.
gamedll calls it with flags `0x37`. `SCollision` is 0x40 bytes: six floats (point, probably
normal), pointers at `0x18`, `0x28`, `0x30`, words at `0x38`/`0x3a` (`0x38 == 3` is skipped by one
caller; a surface/material class). F8 now fires a ray ahead and logs which pointer is the object.
Used now for: thrown/held actors stopping at walls (on by default), and a line-of-fire check on
GMod hits (logged only; `WallsBlockShots=1` to enforce).

### 4.3 Physics bodies
`IPhBody::AddForce`, `AddForceAtPos`, `SetLinearVel`, `SetPosition` exist; `IPhGeom::GetBody` and
`IControlObject::PhysicsBind` lead to them. Not used yet: DL physics props could be pushed by GMod
props/physgun this way once the bind → body path is mapped.

### 4.4 Skeleton
`IModelObject` exposes the whole skeleton: `GetElementsNumber`, `GetElementNameCStr`,
`IsElementABone`, `GetElementWorldPos/Matrix`, `SetElementWorldPos/Matrix`,
`ElementSetWorldMatrixFromQuatPos`, `GetBoneIDFromMeshElem`, `GetMeshElemFromBoneID`,
`SetDontApplyAnim`, `ResetBonesToReferenceFrame`. The current build logs one zombie's full
skeleton (names, bone flags, positions) on its first hit. `EnableElementPhysics` only toggles a
flag (`0x8000000`) on one element's record; it isn't a ragdoll switch.

---

## 5. The script method system (biggest find)

Every game object is a `CRTTIObject` (actors: `+0x28`) and the game registers script methods by
name. The engine exports:
- `CRTTIObject::FindMethod(name)` → `CRTTIMethod*`; `CRTTI::GetMethods`, `CRTTIMethod::GetName`
- `CRTTIVoidMethod::CallVoid(obj)`, `CRTTI1ArgMethod::Call1Arg(obj, arg)`, `Call2Arg`
- `CRTTIObject::CallVoidMethod(name, ...)`

`tools/re/methods2.py` lists the 457 no-argument methods gamedll registers (210 classes,
`docs/re-data/methods_by_class.txt`). Useful ones:

| Class | Methods |
|---|---|
| `AI` | `Kill`, `CheckObjectBindedTo` |
| `Actor` | `TakeDamageToKill` |
| `HumanAI` | `DeleteAfterDie`, `EnableForceUpdateEllipsoidParams` |
| `HumanAIVis` | `DisableElementsRagdoll`, `HideAfterDeath`, `PlayFXAfterDeath` |
| `HumanLogicModule` | `PerformDeathEffectExplode` |
| `ExplodingObject` | `Explode`, `ApplyDamageInRadius` |
| `Grenade`, `ThrowableRock`, `Decoy` | `MakeDamage` |
| `LevelDI` | `DelayedShowDeveloperQuickMenu`, `DelayedShowDeveloperMenuList`, `DelayedShowFavDeveloperMenu` |

GModLight calls `TakeDamageToKill`/`Kill` (void methods only; the method's vtable is checked first).
Methods with arguments are registered another way (templates inlined); finding those is the next
step to calling things like `SetRagdollBehavior` (string present in gamedll).

---

## 6. Spawning, effects, sound

- `IGSObject::CreateObject(const CRTTI&, bool, const string* name)` creates an object of any
  registered class. gamedll's CRTTI objects (RVA): ExplodingObject `0x1c858c0`, Grenade
  `0x1cb2b60`, MolotovCocktail `0x1cb3130`, Flare `0x1cb2970`, DeveloperGrenade `0x1cb2370`,
  ThrowableObject `0x1cb40c0`, ThrowableRock `0x1cb42b0`, Decoy `0x1cb1f90`, PhysicsObject
  `0x1c8dce0`, DeadRagdoll `0x1ccfd50`, Actor `0x1ccccb0`, PlayerDI `0x1c991a0`.
  A bare object likely needs its fields (mesh, radius, damage) set before `Explode`; their field
  names are in gamedll's RTTI field registrations (`m_...` strings).
- `IControlObject::PlaySound3D(name, …)` / `PlaySound3DAtPosition` play DL's own sounds.
- No exported effect (FX) spawner was found on `ILevel`/`IControlObject`.

---

## 7. Plans, ranked

### 7.1 Real ragdolls for thrown/dead zombies (high impact)
GMod side: make the stand-in a `prop_ragdoll` (a ValveBiped model) instead of a box while held or
dead. DL side: `SetDontApplyAnim(true)` on the zombie, then each frame copy GMod's bone matrices to
DL's skeleton with `SetElementWorldMatrix`, mapped by name (DL's names come from the skeleton log;
ValveBiped's are fixed). On release, either keep driving the bones until the ragdoll sleeps, or
`Kill` and let DL's own death ragdoll take over at that pose.

### 7.2 Native explosions (medium)
For GMod blasts (RPG, grenades, explosive barrels), `CreateObject(ExplodingObject)` at the blast
point and call `Explode` / `ApplyDamageInRadius`, after filling its radius/damage fields. Until
then, GMod's own blast damage already reaches zombies in range through their stand-ins.

### 7.3 GMod things hidden behind DL walls (medium, involved)
- DL depth: hook `ID3D11DeviceContext::OMSetRenderTargets`, find the scene depth target (back
  buffer size, depth format, most draws), create an SRV on it (DL is deferred, so it very likely
  has `BIND_SHADER_RESOURCE`), linearise with the projection matrix (check for reversed Z).
- GMod depth: `render.UpdateFullScreenDepthTexture()` fills `_rt_ResolvedFullFrameDepth`; draw it
  into a second region of GMod's back buffer (run GMod at double height) and capture both halves.
- Compare per pixel in the overlay shader.
Cheaper partial version: hide GMod pixels where a DL raycast from the camera hits a wall closer
than the GMod object (per object, not per pixel).

### 7.4 Props that collide with DL's world (medium)
GMod has no DL geometry. Options: raycast-based (each GMod prop raycasts its motion through DL each
tick and is stopped at hits; done for thrown zombies already), or build GMod collision around the
player from a grid of DL raycasts (heightfield of the ground; walls from horizontal rays).

### 7.5 Weapons spawning (user asked, later)
DL's developer menus exist as `LevelDI` script methods. Opening one needs the `LevelDI` object
(probably `ILevel`'s engine object; `ILevel::SetEngineObject` takes a `CGSObject`). From there the
dev menu can spawn DL's own items. GMod's spawn menu Weapons tab already works for GMod weapons.

### 7.6 Hardening
- Thread safety: actor scans, damage and pins run on the thread that presents frames. If that
  isn't DL's game thread, move game calls into a hook on a per-frame game function (candidates:
  `IGSObject::EnableOnPostUpdateHandler` callbacks, or the `GetActiveLevel` caller).
- Handles are raw pointers; only actors seen in the latest scan are touched.
- Co-op: refuse to act when `IGame::AreDataAuthenticatedToPlayMultiplayer` or a session is active.

---

## 8. Things that cost time, so they don't again

- The ASI loader as `dsound.dll` crashed Epic Online Services; `xinput1_3.dll` works.
- Source throttles to 20 fps when unfocused; GModLight swallows focus-loss messages in GMod.
- With `r_drawworld 0` the opaque-renderables hooks never run; render the scene in `RenderScene`.
- Hidden GMod's `Present` blocks on an off-screen window; skip it once captured.
- Dying Light reads input as raw input via `GetRawInputData`; system-wide low-level hooks were
  removed (they can stall all input if their thread does). Input is polled per frame instead.
- GMod's spawn menu pulled focus from DL; `SetForegroundWindow`/`BringWindowToTop` are blocked in
  GMod once hidden.
- Lua `RunConsoleCommand("+attack")` works; overwriting `cmd:SetButtons` broke weapon selection.
- The physgun's own release velocity is wild when its target is pressed into the floor; throws
  use the held box's recent motion.

## 9. Round 5 (first full in-game test of damage)

- **Damage works**, mostly. `IControlObject::TakeDamage` with the weapon-style
  `SDamageInfoDi` applied 16 times; `AI::Kill` killed every time. But the **first
  hit on each actor faulted** (5 of 5 new actors) and the second applied. Probably
  a first-reaction path (an idle AI noticing its attacker) reads a field we leave
  zero. `LogFault` now logs module+offset, the address read, registers and the
  game frames on the stack, to find that field in the disassembly.
- **Line-of-fire "WALL" was a false positive**: the ray hits the actor's own
  collision (`+0x18` is the `HumanAI`). Actor hits no longer count as walls.
- **The skeleton dump works**: 319 elements on a zombie (`bip01`, `pelvis`,
  `spine`..`spine3`, `l_upperarm`, `boneragarml00`..., fingers, sleeve cloth).
  The `boneragarm*` / `bonerag*` elements are the ragdoll's own bones.
- **Raytrace normals**: `RayHit.floats[0..2]` is the surface normal (walls gave
  `(0,0,1)` facing the camera, the ground `(0,1,0)`). Used for probes.
- **FOV**: `render.RenderView`'s `fov` is the real horizontal fov, unlike
  `CalcView`'s 4:3 one. Passing the 4:3 value zoomed GMod ~1.8x on 21:9 (boxes
  swung twice as far as DL's view, guns filled the screen). Verified with
  `fakehost --fovtest`: the box lands 362 px off centre, 359 predicted.
- **Render bounds**: `gml_proxy`'s model is a small cube at the feet, so the box
  wasn't drawn (or client-traced) when the feet were off screen. `SetRenderBounds`.
- New in the bridge (v5): `SceneLight` (DL's average picture colour, mipped on
  the GPU, read back two samples late) lights GMod's viewmodel; `Probes` let GMod
  trace segments through DL's world (rockets explode, grenades bounce, bolts
  stick, bullets spark).

## 10. Round 6

- **TakeDamage, how it really works** (engine 0x13e310): clones the info via its
  vtable slot 1, victim = `[[ctrl+8]+0xa0]`, calls the victim's vtable slot 17,
  mirrors to a network event, deletes the clone. The attacker is read as
  `[[attacker+0x20]+0x40]` (flags). Handles from `FindObjectsInRadius` are
  `IControlObject*` (complete + 0x18); slot 20 of *that* vtable is TakeDamage
  (a gamedll thunk to the engine export). Slot 20 of the primary vtable (+0) is
  an `IModelObject` skin setter, so never call through the complete object.
- **Game tick**: `ILevel::TimerUpdate` (engine 0x2a0300, a thin wrapper) is
  called once per update from gamedll's main update (0x12192cf). Hooked to run
  actor work on the game thread.
- **Skeleton names**: `head`, `neck`, `pelvis`, `spine`..`spine3`, `l_foot`,
  `r_foot`, `l_toebase`; standing zombie: feet→head bone 1.65 m.
- **Effects/lights/co-op classes** (for later): `CFXEmitter`, `FXInstance`,
  `FXEmitterFireSmoke`, `CDecal`/`CDecalMgr`, `FireLightObject`, `CFXLight`,
  `FxBulletVis`, `FlashlightModule`, `SessionCooperative(DI)`,
  `CScriptConditionCoopNum`. No engine exports for FX/decals/lights: they're
  objects, so `CreateObject` + script methods.
- **Frame transfer**: GMod finds the drawn rectangle with SSE2 (exact key
  compare) and copies only that; DL uploads it with `UpdateSubresource` and
  clips outside it. 2560x1072 with 4x MSAA holds 100 fps; 0% copied in plain
  play, ~25–50% with the physgun out.

## 11. Round 7

- **First-hit fault, cause**: engine 0x48e0c0 with a null object, reached from
  `IModelObject::SetSkin` / `SetSkinNoCharacterPreset` (engine 0x158b50 via
  0x1469a0/0x146a00). A zombie's first damage switches its skin (wounds), and
  that ran from Present, not the game thread.
- **`ILevel::TimerUpdate` isn't a frame tick**: called once around a level load.
  The frame tick is now the window thread's `PeekMessage` (in-process hook, once
  per frame, between frames). Log: `threads: Present N, window M`.
- **Why held zombies snapped back**: the public setters go record (`[ctrl+8]`)
  → transform node (`[record+0xe8]`) → a transform table (node index
  `[node+0x28]&0xfff`, world matrices at `[[node+0x20]+0x30]`, local at
  `+0x20`, flags at `+0x40`). The AI also uses `SetWorldPosition` (record-level
  0x136220), `SetLocalPosition` (0x136030) and `SetWorldPosDir`, unhooked until
  now. Pins now hook the node writers 0x1329c0 / 0x132a50 / 0x132910, both record
  writers and SetWorldPosDir (each checked against its first 16 bytes).
- **Game objects**: `IGSObject+0x20` is the engine record (shared with
  `IControlObject+8`); record `+0x20` = game object, `+0x30` = next in the
  level's list, `+0xa0` = the IControlObject (model). `ILevel::GetObjectByID`
  finds the list's start.
- **Kyle's first person**: arms are `PlayerFppVis` (a game object, not a child
  of `PlayerDI`); held things are `WeaponVis`, `ItemVis`, `FireWeaponVis`,
  `DetectorWeaponVis`. Hidden through their models within 3 m of the camera.
- **Latency**: DL timestamps each camera (QPC, shared clock); GMod draws from
  where the camera will be after the measured frame age; DL logs the remainder.

## 12. Round 9 (from the round 8 log)

- **Stutter**: the arm-hiding walk took ~600 ms for 41,070 game objects, every
  2 s, on the game thread. `ClassName`/`CastTo` did four `VirtualQuery` calls
  each. Now: vtable/RTTI pointers are checked against the game modules' address
  ranges, reads are protected by SEH only; the walk runs once per player object.
  `WorldWork` logs any frame over 8 ms with a breakdown.
- **The walk found none of `PlayerFppVis`/`WeaponVis`/`ItemVis`** among those
  41,070 objects. New: the objects the player object points to (two levels) are
  searched every second, and one-time logs list every related class in the level
  and every related pointer in the player object, with offsets.
- **Held zombies are moved by ~8 threads** (job system) via the node world
  setter, `SetWorldXformNoPropagate` and `SetWorldPosition`. Snap-backs happened
  ~50 ms after release (3-12 m): the AI's own copy of its position. The pin hooks
  now remember the position the game last tried to set; on release, copies of it
  in the actor and the objects it points to are overwritten (`[Npcs] ReleaseFix`).
- **Frame age** 25-38 ms in game (13 ms best); movement error avg 0-4 cm, max
  30-40 cm (starts/stops, and the stalls).
- **Window thread = Present thread** (2632): DL renders on its main thread, so the
  "game thread" tick and Present are the same thread here.

## 13. Round 10

- **Kyle's arms**: `PlayerDI + 0x338` -> `PlayerFppVis` (zombies: `HumanAI + 0x338`
  -> `ZombieAIVis`). `PlayerFppVis` bases: BodyVis/HumanVis/PlayerVis/IGSObject
  at +0, `FakeModelObject` at +0x40, `IVis<ActorDI>` at +0x48, `ICameraTarget`
  at +0x2cb0. Its engine record has no model at +0xa0, so its real model is
  found by following its pointers to `IModelObject`s near the camera.
- **Camera**: `IBaseCamera` impl (`[cam+8]`): view matrix (world→camera) at
  +0x10, camera-to-world at +0x40 (columns left, up, back, position — what
  GetLeft/Up/ForwardVector/GetPosition read), projection at +0x70, combined at
  +0xb0, shakeable flag at +0x33c. GModLight now derives the camera from the
  view matrix (what's rendered; shake and head bob included) and logs how far it
  is from the camera object every 10 s.
- **Release fix works**: the AI's position copy lives in `ZombieAIVis`
  (`HumanAI + 0x338`); rewriting it on release stopped the snap-backs.
- **GMod interpolation** was 100 ms (default `cl_interp 0.1`): moving things in
  GMod's frame (physgun beam end, thrown props) trailed. Now 15 ms.
- **Driven actors** are extrapolated along GMod's velocity between its 66 Hz
  updates (up to 35 ms), every DL frame.

## 14. Round 11

- **Camera**: in game, the view matrix and the camera object agree to 0.03 deg, so
  shake/head bob wasn't the grenade drift. The renderer's combined matrix
  (`impl+0xb0`) is now decoded back into a view (each storage order tried; tested
  offline against all three plausible conventions, error 1e-15) and used as the
  camera of the frame on screen. Logged every 10 s vs the view matrix.
- **GMod frame/camera pairing**: frames are stamped (5 blocks of 4x4 in the
  top-left, binary: GMod's colour processing shifts exact values by a few
  levels) and matched to their camera at capture. Result: the pairing was always
  right ("0 would have had another frame's"). fakehost's box-vs-camera check
  (`BoxReprojectionError`) shows ~6-10 px of measurement bias at any turn speed,
  i.e. GMod's images match their cameras.
- **Kyle's arms**: `PlayerFppVis +0x50` -> the PlayerDI model (the first-person
  body), `+0x1020` -> the held `WeaponVis`, `+0x1040` -> a `RopeVis`. The game
  calls `EnableRendering` from ~10 places (and `HideElement`/`UnhideElement`),
  which brought the arms back while climbing; `EnableRendering` is hooked and
  refuses to show force-hidden models while GMod's hands are out.
- **GMod**: `-tickrate 100`, update/cmd rate 100, interpolation 10 ms.

## 15. Round 12

- **Viewmodel**: the "gap at the bottom" is the cut-off end of GMod's arm model
  coming into view when the gun rises, pushes forward or tilts up (sway/inertia).
  Those directions are now clamped (it can still dip, pull back and swing
  sideways), weapons' own `GetViewModelPosition`/`CalcViewModelView` are applied
  on top, and the viewmodel fov is 1.15x (`gmodlight_viewmodel_scale`): more of
  the arms and sleeves, as compared in fakehost renders.
- **Dying Light's weapon HUD**: `IUIElement::SetVisible/IsVisible` exported;
  HUD components are `Hud*`/`HUD*` game objects (182 classes), the weapon ones
  being `HudPrimaryWeaponIndicator`, `HudSecondaryWeaponIndicator`,
  `HudHeldItem`, `HudWeaponSelector`, `HudCrosshair` (also `HudQuickActions`,
  `HudAmmoSelection`). Elements owned by exactly one target component are
  hidden; `SetVisible` is hooked to keep them hidden.

### If the grenade still drifts when turning (next hypotheses)
1. The log line `renderer's camera vs view matrix: up to X deg` says whether
   Present sees the next frame's camera. If X is ~0 while turning, that wasn't it.
2. DL may apply TAA jitter / a separate "render camera" with motion-blur reprojection;
   compare against the frame's actual constant buffer (hook
   `ID3D11DeviceContext::UpdateSubresource`/`Map` on the per-view CB) to read the
   exact view-projection the GPU used.
3. Translation: the composite only re-projects rotation. Head pivot while turning
   moves the eye a few cm; near objects (a grenade at 1-2 m) shift. Fix: render
   GMod depth (`render.SetWriteDepthToDestAlpha` into an A8R8G8B8 RT) and do a
   depth-aware re-projection in the composite shader.

## 16. Round 13

- **Renderer camera = view matrix** in game (<= 0.06 deg): Present doesn't see the
  next frame's camera. GMod's images match their cameras (stamps). The remaining
  drift of near things is **parallax**: the eye moves (head pivot, bob, walking)
  during GMod's ~30 ms frame age, and re-projection was rotation only.
- **Depth-aware re-projection**: GMod's back buffer is A8R8G8B8. `SetWriteDepth
  ToDestAlpha` doesn't reach our RenderView, so GMod draws distances itself: alpha
  cleared to 0, each world entity's screen rectangle filled with
  `8 + z/25m*246` (far first), the viewmodel/hands forced back to 0 with a
  ZERO/ZERO alpha blend. 0-7 and 255 = unknown (rotation only). The composite
  shader inverts the warp with 4 fixed-point steps (TAA-style).
  fakehost `--parallax-move M`: GMod's old frame warped to a camera moved M aside
  vs GMod's own render from there (IoU of drawn pixels): 10 cm 0.715 -> 0.881,
  40 cm 0.417 -> 0.770.
- **GMod is D3D9Ex**: GPU thread priority +7 and max frame latency 1 accepted
  (its frames queued behind Dying Light's on a ~95% busy GPU).
- **Stutter**: the weapon-HUD pass rebuilt itself and logged every frame when it
  found nothing (7,000 lines); now once per set of components.

## 17. Round 14

- **Frame cap + lockstep**: Dying Light at ~160 fps kept the GPU ~95% busy;
  GMod's frames arrived 20-40 ms old at irregular times (stutter, uneven
  alignment) and GMod only managed ~60 of 100 fps. Now: a 60 fps limiter in DL's
  Present hook (high-resolution waitable timer + 1 ms spin, after Present), and
  GMod in lockstep: DL sets `Local\GModLight_frame` after publishing each camera;
  GMod waits on it after each captured frame (50 ms timeout),
  `mat_queue_mode 0`. fakehost at 60 Hz: GMod 59 fps, frame age 16.7 ms avg
  (max ~18-23), waiting ~13.6 ms per frame (its own frame takes ~3 ms).
- **Hitch log**: DL frames longer than 1.5x the cap's period are logged with the
  time spent in our world work, scene-light sample, upload and composite.
