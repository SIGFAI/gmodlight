// Thin wrappers over Chrome Engine 6 functions exported by engine_x64_rwdi.dll.
#pragma once
#include <cstdint>
#include "../../shared/bridge.h"

namespace gml::eng {

struct vec3 { float x, y, z; };
struct mtx34 { float m[3][4]; };  // rows; translation in column 3 (verified at runtime, see log)

bool Init();                 // resolve exports and install hooks; call once
void* Level();               // active ILevel, captured from game calls; null until in game

bool ReadCamera(Camera& out);
bool IsPaused();             // level timer frozen (pause menu, loading)
// Camera from the renderer's view matrix (with shake/head bob) instead of the camera object's.
void SetUseViewMatrix(bool on);
// Largest difference between the two since the last call; times the view matrix looked wrong.
void TakeViewMatrixStats(float& maxDeg, float& maxCm, int& insane);
// Camera from the renderer's combined (projection x view) matrix: the frame actually drawn.
void SetUseCombined(bool on);
void TakeCombinedStats(float& maxDeg, float& maxCm, int& used, int& frames);

// Objects within `radius` meters of `center`. Returns count written to `out`.
int FindNearby(const vec3& center, float radius, void** out, int max);

const char* ClassName(void* obj);  // MSVC RTTI type name, or "?" if unknown
void* Parent(void* obj);           // IControlObject::GetParent, or null
bool GetWorldXform(void* obj, mtx34& out);
void SetWorldXform(void* obj, const mtx34& m);

// Cast between base classes of one object by MSVC RTTI type name (".?AVIModelObject@@").
void* CastTo(void* obj, const char* typeName);
int Children(void* obj, void** out, int max);  // IControlObject::GetChildren
// How the engine names an object in SDamageInfo (attacker/victim): [[ctrl+8]+0xa0].
void* DamageIdentity(void* obj);
// Damage an object through its own TakeDamage, the way DL's explosions do.
bool TakeDamage(void* obj, void* attacker, float amount, const vec3& pos, const vec3& dir, uint32_t dlType,
                float impulse = 135.0f, int* boneOut = nullptr);
// Nearest skeleton bone (mesh element) of an actor to a point; -1 if none.
int NearestBone(void* obj, const vec3& p, vec3* bonePos = nullptr);
void LogSkeleton(void* obj);
// World positions of the head bone and between the feet; false if the model lacks them.
bool GetPose(void* obj, vec3& head, vec3& feet);
// The game's named, no-argument script methods (e.g. AI "Kill").
bool HasScriptMethod(void* obj, const char* name);
bool CallScriptMethod(void* obj, const char* name);
bool SetRendering(void* obj, bool on);         // IModelObject::EnableRendering
// While these stay listed, the game's own EnableRendering(true) on them is refused.
void SetForceHidden(void* const* objs, int n);
// How many times the game tried to show a force-hidden model since the last call.
int TakeBlockedShows();
// Dying Light UI elements (IUIElement::SetVisible / IsVisible), and a force-hidden list
// the game's own SetVisible(true) can't undo.
bool UIVisible(void* elem);
bool SetUIVisible(void* elem, bool on);
void SetForceHiddenUI(void* const* elems, int n);
struct RayHit {
    bool hit = false;
    vec3 pos{};          // hit point (or the end point if nothing was hit)
    float dist = 0;
    float floats[6]{};   // SCollision 0x00..0x14
    void* p18 = nullptr; // SCollision pointers; which is the hit object is logged by F8
    void* p28 = nullptr;
    void* p30 = nullptr;
};
// Dying Light's own raycast through the level, from any game object (the player).
bool Raytrace(void* fromObj, const vec3& from, const vec3& to, RayHit& out, void* ignore = nullptr);

// Actors whose transform the engine must keep at these positions, whoever sets it.
void SetPins(void* const* objs, const vec3* pos, int n);

// Every game object (IGSObject) in the active level, walking the engine's list.
// `anyGso` is a known one (e.g. the player's), a second starting point.
int AllGameObjects(void* anyGso, void** out, int max);
// The engine model (IControlObject) a game object drives, or null.
void* ModelOf(void* gso);
// Where the game last tried to put a pinned actor (its AI's own idea), once.
bool TakeGamePosition(void* obj, vec3& out);
// Overwrite the game's own copies of that position (in the actor and the objects
// it points to) with `to`, so its AI carries on from where GMod let it go.
int MoveGameCopies(void* obj, const vec3& from, const vec3& to);
// Polymorphic objects whose pointers are stored in the first `bytes` of obj
// (e.g. what the player object owns), with the offsets they're stored at.
int ObjectsReferencedBy(void* obj, size_t bytes, void** out, uint32_t* offsets, int max);

// Called after every ILevel::TimerUpdate, on the game's update thread.
using TickCallback = void (*)();
bool InstallTickHook(TickCallback cb);

// Logs every object near the camera with its class and position. Debug key.
void DumpNearby(float radius);

}  // namespace gml::eng
