// GModLight shared-memory bridge between Dying Light (host, draws the game) and
// Garry's Mod (runs hidden, supplies spawn menu, physgun and physics).
//
// Dying Light owns and creates the mapping; Garry's Mod opens it.
// All positions in this file are in Dying Light space: meters, Y up.
#pragma once
#include <cstdint>

namespace gml {

constexpr wchar_t kShmName[] = L"Local\\GModLight_v1";
// Auto-reset event Dying Light sets once per frame, after publishing its camera.
// GMod waits on it at the end of each of its frames (lockstep): one GMod frame per
// DL frame, started as soon as the camera is there, so every frame is equally fresh.
constexpr wchar_t kFrameEventName[] = L"Local\\GModLight_frame";
constexpr uint32_t kMagic = 0x544C4D47;  // "GMLT"
constexpr uint32_t kVersion = 8;

constexpr uint32_t kMaxFrameW = 2560;
constexpr uint32_t kMaxFrameH = 1440;
constexpr uint32_t kFrameBytes = kMaxFrameW * kMaxFrameH * 4;
constexpr uint32_t kFrameBuffers = 3;   // triple buffered, see FrameSlots
constexpr uint32_t kMaxNpcs = 64;
constexpr uint32_t kMaxInputEvents = 128;

struct Vec3 { float x, y, z; };

// Dying Light -> GMod, written every DL frame.
struct Camera {
    Vec3 pos;
    Vec3 fwd;
    Vec3 up;
    Vec3 left;          // DL's own left vector; tells GMod whether DL's axes are mirrored
    float fovDeg;       // as reported by IBaseCamera::GetFOV
    float aspect;       // as reported by IBaseCamera::GetAspect
    uint32_t viewW, viewH;  // DL back buffer size
    uint32_t valid;
    uint32_t seq;
    double time;        // when DL read it: QueryPerformanceCounter seconds (same clock in both processes)
};

// What GMod should be doing right now. Set by DL from its key bindings.
enum Mode : uint32_t {
    kModePlay = 0,         // plain Dying Light; GMod only draws props
    kModeSpawnMenu = 1,    // spawn menu (Q) open; mouse and keyboard go to GMod
    kModeGMod = 2,         // "GMod hands": DL moves and looks, GMod gets mouse buttons,
                           // wheel, number keys, R, E, Z, and shows its weapons and HUD
    kModeContextMenu = 3,  // context menu (C) open; mouse and keyboard go to GMod
};

enum InputType : uint32_t {
    kInMouseMove = 1,   // a = x, b = y (pixels in GMod frame space)
    kInMouseDown = 2,   // a = button (0 left, 1 right, 2 middle)
    kInMouseUp = 3,
    kInWheel = 4,       // a = delta (+1 up, -1 down)
    kInKeyDown = 5,     // a = Windows virtual key
    kInKeyUp = 6,
    kInChar = 7,        // a = UTF-16 code unit
};

struct InputEvent { uint32_t type; int32_t a, b; };

// Ring buffer, DL writes head, GMod writes tail.
struct InputQueue {
    volatile uint32_t head;
    volatile uint32_t tail;
    InputEvent events[kMaxInputEvents];
};

// DL -> GMod: nearby actors that GMod mirrors as physics proxies.
enum NpcFlags : uint32_t {
    kNpcAlive = 1,
    kNpcHeldByGMod = 2,  // echoed back so DL knows GMod still owns it
    kNpcBlocked = 4,     // a thrown actor hit Dying Light's world; pos is where it stopped
    kNpcPosed = 8,       // head and feet below are from its skeleton
};

struct Npc {
    uint64_t handle;     // DL object pointer, used only as an id
    Vec3 pos;            // feet
    float yaw;
    float height;
    uint32_t flags;
    Vec3 head;           // "head" bone (base of the skull), if kNpcPosed
    Vec3 feet;           // between the feet, if kNpcPosed
    char cls[48];        // C++ class name, for debugging
};

// GMod -> DL: actors GMod is currently driving (physgunned or flying).
struct Driven {
    uint64_t handle;
    Vec3 pos;
    Vec3 vel;
    float impact;        // largest impact speed this frame, m/s, 0 if none
};

// GMod -> DL: things that happened in GMod that DL has to act out.
enum EventType : uint32_t {
    kEvDamage = 1,    // GMod damaged an actor's stand-in: handle, amount, pos = hit point, dir
    kEvKill = 2,      // GMod's health for the actor ran out: kill it the game's own way
};

// Dying Light's EDamageType values (from gamedll's enum registration).
enum DLDamageType : uint32_t {
    kDLCut = 1, kDLBullet = 2, kDLBlast = 3, kDLElectric = 5, kDLHeat = 6, kDLFire = 0xb,
    kDLPunch1 = 0xc, kDLPunch2 = 0xd, kDLPunch3 = 0xe, kDLImpact = 0x1e,
};

struct Event {
    uint32_t type;
    float amount;
    uint64_t handle;
    Vec3 pos;
    Vec3 dir;
    uint32_t dlDamageType;  // DLDamageType
};

constexpr uint32_t kMaxEvents = 256;

// Ring buffer, GMod writes head, DL writes tail.
struct EventQueue {
    volatile uint32_t head;
    volatile uint32_t tail;
    Event events[kMaxEvents];
};

// Debug switches DL sets for GMod (F9).
enum DebugFlags : uint32_t {
    kDebugProxies = 1,  // draw actor stand-ins
};

// Triple-buffered frame. Writer picks a slot that is neither `latest` nor
// `reading`, fills it, then publishes it as `latest`.
// The camera GMod rendered a frame with, so DL can re-project it to its current
// camera (GMod's frame is a frame or two behind; without this it trails when turning).
struct FrameCamera {
    Vec3 fwd, up, left;
    uint32_t valid;
    Vec3 pos;       // where GMod drew from (DL space; predicted ahead, see GML.RenderCamera)
    double time;    // DL's camera sample it started from (Camera::time)
    uint32_t id;    // counts RenderScene calls; the self-test stamps it into the image
};

struct FrameSlots {
    volatile uint32_t latest;   // slot index of newest complete frame, or ~0u
    volatile uint32_t reading;  // slot the reader is copying, or ~0u
    volatile uint32_t seq;      // bumps on every published frame
    uint32_t w, h, stride;      // of `latest`
    FrameCamera cam[kFrameBuffers];  // per slot
    // Per slot: the part of the frame that isn't chroma key (x0, y0, x1, y1;
    // x1/y1 exclusive, 2 px of key around it). Only that part of the slot is
    // written; the rest is stale and must be treated as key. Empty if x1 <= x0.
    uint32_t rect[kFrameBuffers][4];
};

// DL -> GMod: average colour of the middle of DL's picture (0..1, as displayed),
// so GMod can light its weapons like the scene around them (dark at night).
struct SceneLight {
    float r, g, b;
    volatile uint32_t seq;
};

// GMod -> DL: line segments to trace through Dying Light's world, for GMod
// things that have to hit DL's walls and ground (rockets, grenades, bullets).
// GMod fills probes and bumps seq; DL traces them and answers with the hits,
// setting resultSeq to the seq it answered.
constexpr uint32_t kMaxProbes = 32;
struct Probe { uint32_t id; Vec3 from, to; };
struct ProbeHit { uint32_t id; float dist; Vec3 pos; Vec3 normal; };
struct Probes {
    volatile uint32_t seq;
    uint32_t count;
    Probe probes[kMaxProbes];
    volatile uint32_t resultSeq;
    uint32_t resultCount;
    ProbeHit results[kMaxProbes];  // only the probes that hit something
};

struct Header {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t dlHeartbeat;   // bumped by DL every frame
    volatile uint32_t gmHeartbeat;   // bumped by GMod every frame
    uint32_t dlPid;
    uint32_t gmPid;

    Camera cam;
    volatile uint32_t mode;          // Mode
    volatile uint32_t gmodReady;     // GMod in a map with the addon running

    volatile uint32_t npcSeq;
    uint32_t npcCount;
    Npc npcs[kMaxNpcs];

    volatile uint32_t drivenSeq;
    uint32_t drivenCount;
    Driven driven[kMaxNpcs];

    InputQueue input;
    EventQueue events;
    volatile uint32_t debugFlags;
    // DL -> GMod: how old GMod's frames are when DL shows them (seconds since the
    // camera sample they were drawn from), averaged. GMod draws that far ahead.
    volatile float latency;
    SceneLight scene;
    Probes probes;
    FrameSlots frame;
};

constexpr uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
constexpr uint64_t kFrameOffset = AlignUp(sizeof(Header), 4096);
constexpr uint64_t kShmSize = kFrameOffset + uint64_t(kFrameBytes) * kFrameBuffers;

inline uint8_t* FramePtr(void* base, uint32_t slot) {
    return static_cast<uint8_t*>(base) + kFrameOffset + uint64_t(kFrameBytes) * slot;
}

// Chroma key GMod clears its frame to; DL treats it as transparent.
constexpr uint8_t kKeyR = 255, kKeyG = 0, kKeyB = 255;

}  // namespace gml
