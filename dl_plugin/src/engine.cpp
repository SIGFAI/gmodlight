#include "engine.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "log.h"

namespace gml::eng {
namespace {

// Member functions are plain x64 calls with `this` first. Class types returned
// by value come back through a hidden pointer passed right after `this`.
using GetActiveLevel_t = void* (*)(void* game);
using GetActiveCamera_t = void* (*)(const void* level);
using CamVec_t = vec3* (*)(const void* cam, vec3* ret);
using CamFloat_t = float (*)(void* cam);
using GetXform_t = const mtx34* (*)(const void* obj);
using SetXform_t = void (*)(void* obj, const mtx34* m);
using GetNativeClass_t = const void* (*)();
using LevelBool_t = bool (*)(const void* level);
using FindInRadius_t = bool (*)(void* level, void* outVec, const vec3* center, float radius,
                                const void* rttiFilter, bool flag, int* outCount);

GetActiveLevel_t oGetActiveLevel = nullptr;
GetActiveCamera_t pGetActiveCamera = nullptr;
GetActiveCamera_t oGetActiveCamera = nullptr;
CamVec_t pCamPos = nullptr, pCamFwd = nullptr, pCamUp = nullptr, pCamLeft = nullptr;
CamFloat_t pCamFov = nullptr, pCamAspect = nullptr;
struct mtx44 { float m[4][4]; };
using CamProj_t = const mtx44* (*)(void* cam);
CamProj_t pCamProj = nullptr;
using CamView_t = const mtx34* (*)(void* cam);
CamView_t pCamView = nullptr;
using CamComb_t = const mtx44* (*)(void* cam);
CamComb_t pCamComb = nullptr;
GetXform_t pGetWorldXform = nullptr;
SetXform_t pSetWorldXform = nullptr;
GetNativeClass_t pControlObjectClass = nullptr;
FindInRadius_t pFindInRadius = nullptr;
LevelBool_t pIsTimerFrozen = nullptr;
using GetParent_t = void* (*)(const void* obj);
GetParent_t pGetParent = nullptr;
using GetChildren_t = void (*)(const void* obj, void* outVec);
GetChildren_t pGetChildren = nullptr;
using EnableRendering_t = void (*)(void* modelObject, bool on);
EnableRendering_t pEnableRendering = nullptr;
uintptr_t gGameDll = 0;  // gamedll_x64_rwdi.dll base, for SDamageInfoDi

std::atomic<void*> gGame{nullptr};
std::atomic<void*> gLevel{nullptr};

void* HkGetActiveLevel(void* game) {
    void* level = oGetActiveLevel(game);
    gGame.store(game, std::memory_order_relaxed);
    if (level) gLevel.store(level, std::memory_order_relaxed);
    return level;
}

void* HkGetActiveCamera(const void* level) {
    if (level) gLevel.store(const_cast<void*>(level), std::memory_order_relaxed);
    return oGetActiveCamera(level);
}

template <class T>
bool Resolve(HMODULE mod, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(mod, name));
    if (!out) LOGW("export missing: %s", name);
    return out != nullptr;
}

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<const uint8_t*>(p) + n <=
           reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

}  // namespace

bool InstallPinHooks();
bool InstallRenderHook();
bool InstallUIHook();

bool Init() {
    HMODULE e = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!e) e = LoadLibraryA("engine_x64_rwdi.dll");
    if (!e) { LOGE("engine_x64_rwdi.dll not loaded"); return false; }

    void* getActiveLevel = nullptr;
    void* getActiveCamera = nullptr;
    bool ok = true;
    ok &= Resolve(e, "?GetActiveLevel@IGame@@QEAAPEAVILevel@@XZ", getActiveLevel);
    ok &= Resolve(e, "?GetActiveCamera@ILevel@@QEBAPEAVIBaseCamera@@XZ", getActiveCamera);
    ok &= Resolve(e, "?GetPosition@IBaseCamera@@QEBA?BVvec3@@XZ", pCamPos);
    ok &= Resolve(e, "?GetForwardVector@IBaseCamera@@QEBA?BVvec3@@XZ", pCamFwd);
    ok &= Resolve(e, "?GetUpVector@IBaseCamera@@QEBA?BVvec3@@XZ", pCamUp);
    Resolve(e, "?GetLeftVector@IBaseCamera@@QEBA?BVvec3@@XZ", pCamLeft);
    Resolve(e, "?GetProjectionMatrix@IBaseCamera@@QEAAAEBVmtx44@@XZ", pCamProj);
    Resolve(e, "?GetViewMatrix@IBaseCamera@@QEAAAEBVmtx34@@XZ", pCamView);
    Resolve(e, "?GetCombinedMatrix@IBaseCamera@@QEAAAEBVmtx44@@XZ", pCamComb);
    ok &= Resolve(e, "?GetFOV@IBaseCamera@@QEAAMXZ", pCamFov);
    ok &= Resolve(e, "?GetAspect@IBaseCamera@@QEAAMXZ", pCamAspect);
    ok &= Resolve(e, "?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ", pGetWorldXform);
    ok &= Resolve(e, "?SetWorldXform@IControlObject@@QEAAXAEBVmtx34@@@Z", pSetWorldXform);
    ok &= Resolve(e, "?FindObjectsInRadius@ILevel@@QEAA_NPEAV?$vector@PEAVIControlObject@@@ttl@@AEBVvec3@@MPEBVCRTTI@@_NPEAH@Z", pFindInRadius);
    Resolve(e, "?GetNativeClass@IControlObject@@SAPEBVCRTTI@@XZ", pControlObjectClass);
    Resolve(e, "?IsTimerFrozen@ILevel@@QEBA_NXZ", pIsTimerFrozen);
    Resolve(e, "?GetParent@IControlObject@@QEBAPEAV1@XZ", pGetParent);
    Resolve(e, "?GetChildren@IControlObject@@QEBAXPEAV?$vector@PEAVIControlObject@@@ttl@@@Z", pGetChildren);
    Resolve(e, "?EnableRendering@IModelObject@@QEAAX_N@Z", pEnableRendering);
    gGameDll = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    LOGI("gamedll at %p", reinterpret_cast<void*>(gGameDll));
    pGetActiveCamera = reinterpret_cast<GetActiveCamera_t>(getActiveCamera);
    if (!ok) return false;

    // The engine never hands us IGame/ILevel directly, so catch them as the game asks for them.
    if (MH_CreateHook(getActiveLevel, &HkGetActiveLevel, reinterpret_cast<void**>(&oGetActiveLevel)) == MH_OK &&
        MH_EnableHook(getActiveLevel) == MH_OK)
        LOGI("hooked IGame::GetActiveLevel");
    else
        LOGW("could not hook IGame::GetActiveLevel");
    if (MH_CreateHook(getActiveCamera, &HkGetActiveCamera, reinterpret_cast<void**>(&oGetActiveCamera)) == MH_OK &&
        MH_EnableHook(getActiveCamera) == MH_OK)
        LOGI("hooked ILevel::GetActiveCamera");
    else
        LOGW("could not hook ILevel::GetActiveCamera");
    if (!oGetActiveCamera) oGetActiveCamera = pGetActiveCamera;
    InstallPinHooks();
    InstallRenderHook();
    InstallUIHook();
    return true;
}

void* Level() { return gLevel.load(std::memory_order_relaxed); }

float gCombDiffMaxDeg = 0, gCombDiffMaxCm = 0;
int gCombUsed = 0, gCombFrames = 0, gCombOrder = -1;

// 4x4 inverse (general; Gauss-Jordan). False if singular.
bool Inverse44(const float a[4][4], float out[4][4]) {
    float m[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? a[r][c] : (c - 4 == r ? 1.0f : 0.0f);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        if (std::fabs(m[piv][c]) < 1e-12f) return false;
        if (piv != c)
            for (int k = 0; k < 8; ++k) std::swap(m[c][k], m[piv][k]);
        float d = m[c][c];
        for (int k = 0; k < 8; ++k) m[c][k] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                float f = m[r][c];
                for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c + 4];
    return true;
}

void Mul44(const float a[4][4], const float b[4][4], float out[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r][k] * b[k][c];
            out[r][c] = s;
        }
}

// The view the renderer used, from its combined (projection x view) matrix: the
// view matrix V can already be the next frame's when Present runs. The storage
// order isn't known, so each way of combining is tried and the one that gives
// back a rigid transform close to V is used (and remembered).
bool ViewFromCombined(const mtx44& comb, const mtx44& proj, const mtx34& view, float outV[3][4]) {
    float C[4][4], P[4][4], Pi[4][4], Ct[4][4], Pt[4][4], Pti[4][4], V[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { C[r][c] = comb.m[r][c]; P[r][c] = proj.m[r][c]; Ct[c][r] = C[r][c]; Pt[c][r] = P[r][c]; }
    if (!Inverse44(P, Pi) || !Inverse44(Pt, Pti)) return false;
    auto tryOrder = [&](int order, float out[4][4]) {
        switch (order) {
            case 0: Mul44(Pi, C, out); return;        // C = P V
            case 1: Mul44(C, Pi, out); return;        // C = V P
            case 2: Mul44(Pti, Ct, out); return;      // stored transposed: C^T = P^T V
            default: {                                // C^T = V P^T
                Mul44(Ct, Pti, out);
                return;
            }
        }
    };
    auto rigidAndClose = [&](float M[4][4], bool transposed) {
        // Rows of the 3x3 (or columns, if transposed) must be unit and close to V's.
        float dot = 0;
        for (int i = 0; i < 3; ++i) {
            float l = 0, d = 0;
            for (int k = 0; k < 3; ++k) {
                float x = transposed ? M[k][i] : M[i][k];
                l += x * x;
                d += x * view.m[i][k];
            }
            if (std::fabs(l - 1) > 0.02f) return -1.0f;
            dot += d;
        }
        return dot / 3;  // 1 = identical rotation
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int order = 0; order < 4; ++order) {
            if (pass == 0 && order != gCombOrder) continue;  // the known one first
            tryOrder(order, V);
            for (int tr = 0; tr < 2; ++tr) {
                float sim = rigidAndClose(V, tr == 1);
                if (sim < 0.95f) continue;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) outV[r][c] = tr ? (c < 3 ? V[c][r] : V[3][r]) : V[r][c];
                if (gCombOrder != order) {
                    gCombOrder = order;
                    LOGI("camera: combined matrix decodes as order %d%s", order, tr ? " (transposed)" : "");
                }
                return true;
            }
        }
    return false;
}

bool gUseViewMatrix = true;
bool gUseCombined = true;
float gViewDiffMaxDeg = 0, gViewDiffMaxCm = 0;
int gViewInsane = 0;

void SetUseViewMatrix(bool on) { gUseViewMatrix = on; }
void SetUseCombined(bool on) { gUseCombined = on; }

void TakeCombinedStats(float& maxDeg, float& maxCm, int& used, int& frames) {
    maxDeg = gCombDiffMaxDeg; maxCm = gCombDiffMaxCm; used = gCombUsed; frames = gCombFrames;
    gCombDiffMaxDeg = gCombDiffMaxCm = 0;
    gCombUsed = gCombFrames = 0;
}

void TakeViewMatrixStats(float& maxDeg, float& maxCm, int& insane) {
    maxDeg = gViewDiffMaxDeg; maxCm = gViewDiffMaxCm; insane = gViewInsane;
    gViewDiffMaxDeg = gViewDiffMaxCm = 0;
    gViewInsane = 0;
}

bool ReadCamera(Camera& out) {
    void* level = Level();
    if (!level) return false;
    __try {
        void* cam = oGetActiveCamera(level);
        if (!cam) return false;
        vec3 p, f, u;
        pCamPos(cam, &p);
        pCamFwd(cam, &f);
        pCamUp(cam, &u);
        vec3 l{0, 0, 0};
        if (pCamLeft) pCamLeft(cam, &l);
        out.left = {l.x, l.y, l.z};
        out.pos = {p.x, p.y, p.z};
        // IBaseCamera::GetForwardVector is the camera's +Z axis; the camera looks
        // down -Z. Verified in game: zombies the player was looking at came out
        // 180 degrees "behind" without this, and left = forward x up only holds
        // with it.
        out.fwd = {-f.x, -f.y, -f.z};
        out.up = {u.x, u.y, u.z};
        // The camera object's vectors are its camera-to-world matrix (impl+0x40:
        // columns left, up, back, position). What the renderer draws with is the
        // view matrix (impl+0x10, world-to-camera), which may add camera shake and
        // head bob. If GMod follows the object camera while DL draws with a shaken
        // one, GMod's things float relative to DL's world (round 9: a grenade on the
        // ground "moves with the camera"). Use the view matrix when it's sane.
        if (pCamView && gUseViewMatrix) {
            if (const mtx34* v = pCamView(cam)) {
                const float(*r)[4] = v->m;
                vec3 vl{r[0][0], r[0][1], r[0][2]}, vu{r[1][0], r[1][1], r[1][2]}, vb{r[2][0], r[2][1], r[2][2]};
                vec3 vp{-(r[0][0] * r[0][3] + r[1][0] * r[1][3] + r[2][0] * r[2][3]),
                        -(r[0][1] * r[0][3] + r[1][1] * r[1][3] + r[2][1] * r[2][3]),
                        -(r[0][2] * r[0][3] + r[1][2] * r[1][3] + r[2][2] * r[2][3])};
                auto len2 = [](const vec3& a) { return a.x * a.x + a.y * a.y + a.z * a.z; };
                vec3 vf{-vb.x, -vb.y, -vb.z};
                float dotF = vf.x * out.fwd.x + vf.y * out.fwd.y + vf.z * out.fwd.z;
                float dx = vp.x - p.x, dy = vp.y - p.y, dz = vp.z - p.z;
                float dpos = std::sqrt(dx * dx + dy * dy + dz * dz);
                bool sane = std::fabs(len2(vf) - 1) < 0.01f && std::fabs(len2(vu) - 1) < 0.01f && dotF > 0.94f && dpos < 1.0f;
                float ang = std::acos(std::max(-1.0f, std::min(1.0f, dotF))) * 57.29578f;
                gViewDiffMaxDeg = std::max(gViewDiffMaxDeg, sane ? ang : 0.0f);
                gViewDiffMaxCm = std::max(gViewDiffMaxCm, sane ? dpos * 100 : 0.0f);
                if (!sane) ++gViewInsane;
                if (sane) {
                    out.fwd = {vf.x, vf.y, vf.z};
                    out.up = {vu.x, vu.y, vu.z};
                    out.left = {vl.x, vl.y, vl.z};
                    out.pos = {vp.x, vp.y, vp.z};
                }
                // And the renderer's own: the view inside the combined matrix.
                const mtx44* comb = pCamComb ? pCamComb(cam) : nullptr;
                const mtx44* proj = pCamProj ? pCamProj(cam) : nullptr;
                float rv[3][4];
                ++gCombFrames;
                if (sane && comb && proj && ViewFromCombined(*comb, *proj, *v, rv)) {
                    vec3 cl{rv[0][0], rv[0][1], rv[0][2]}, cu{rv[1][0], rv[1][1], rv[1][2]};
                    vec3 cf{-rv[2][0], -rv[2][1], -rv[2][2]};
                    vec3 cp{-(rv[0][0] * rv[0][3] + rv[1][0] * rv[1][3] + rv[2][0] * rv[2][3]),
                            -(rv[0][1] * rv[0][3] + rv[1][1] * rv[1][3] + rv[2][1] * rv[2][3]),
                            -(rv[0][2] * rv[0][3] + rv[1][2] * rv[1][3] + rv[2][2] * rv[2][3])};
                    float d = std::max(-1.0f, std::min(1.0f, cf.x * vf.x + cf.y * vf.y + cf.z * vf.z));
                    float ex = cp.x - vp.x, ey = cp.y - vp.y, ez = cp.z - vp.z;
                    float cm = std::sqrt(ex * ex + ey * ey + ez * ez) * 100;
                    gCombDiffMaxDeg = std::max(gCombDiffMaxDeg, std::acos(d) * 57.29578f);
                    gCombDiffMaxCm = std::max(gCombDiffMaxCm, cm);
                    if (gUseCombined && cm < 100) {
                        ++gCombUsed;
                        out.fwd = {cf.x, cf.y, cf.z};
                        out.up = {cu.x, cu.y, cu.z};
                        out.left = {cl.x, cl.y, cl.z};
                        out.pos = {cp.x, cp.y, cp.z};
                    }
                }
            }
        }
        out.fovDeg = pCamFov(cam);
        out.aspect = pCamAspect(cam);
        // GetFOV's meaning isn't documented; the projection matrix is exact.
        // Its diagonal is 1/tan(half fov) per axis, whichever way it's stored.
        if (pCamProj) {
            if (const mtx44* pm = pCamProj(cam)) {
                float sx = pm->m[0][0], sy = pm->m[1][1];
                if (sx > 0.05f && sx < 20.0f && sy > 0.05f && sy < 20.0f) {
                    out.fovDeg = 2.0f * std::atan(1.0f / sy) * 57.29578f;  // vertical
                    out.aspect = sy / sx;
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsPaused() {
    void* level = Level();
    if (!level || !pIsTimerFrozen) return false;
    __try {
        return pIsTimerFrozen(level);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Is p inside one of the game's own modules (where vtables and RTTI live)?
// A few compares instead of VirtualQuery: ClassName/CastTo run thousands of
// times per scan, and four VirtualQuery calls each made a 41k-object walk take
// 0.6 s (the round 8 stutter).
bool InGameModule(const void* p) {
    struct Range { uintptr_t lo, hi; };
    static Range ranges[4];
    static int count = -1;
    static bool haveGameDll = false;
    static DWORD lastTry = 0;
    // Until gamedll is loaded (it comes after this plugin), look again now and then.
    if (count < 0 || (!haveGameDll && GetTickCount() - lastTry > 1000)) {
        lastTry = GetTickCount();
        haveGameDll = GetModuleHandleA("gamedll_x64_rwdi.dll") != nullptr;
        int n = 0;
        for (const char* name : {"gamedll_x64_rwdi.dll", "engine_x64_rwdi.dll", "rd3d11_x64_rwdi.dll"}) {
            HMODULE m = GetModuleHandleA(name);
            if (!m) continue;
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
            ranges[n++] = {reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage};
        }
        HMODULE exe = GetModuleHandleA(nullptr);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
        ranges[n++] = {reinterpret_cast<uintptr_t>(exe), reinterpret_cast<uintptr_t>(exe) + nt->OptionalHeader.SizeOfImage};
        count = n;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    for (int i = 0; i < count; ++i)
        if (a >= ranges[i].lo && a + 32 <= ranges[i].hi) return true;
    return false;
}

const char* ClassName(void* obj) {
    // MSVC x64 RTTI: vtable[-1] -> CompleteObjectLocator -> TypeDescriptor name.
    // Reading obj may fault (freed or bad pointer): __except catches it.
    __try {
        if (!obj || (reinterpret_cast<uintptr_t>(obj) & 7)) return "?";
        void** vtbl = *reinterpret_cast<void***>(obj);
        if (!InGameModule(vtbl - 1)) return "?";
        auto* col = reinterpret_cast<const uint32_t*>(vtbl[-1]);
        if (!InGameModule(col) || col[0] != 1) return "?";
        uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];
        auto* td = reinterpret_cast<const char*>(base + col[3]);
        if (!InGameModule(td)) return "?";
        return td + 16;  // ".?AVClassName@@"
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

bool GetWorldXform(void* obj, mtx34& out) {
    __try {
        const mtx34* m = pGetWorldXform(obj);
        if (!m) return false;
        out = *m;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetWorldXform(void* obj, const mtx34& m) {
    __try {
        pSetWorldXform(obj, &m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int FindNearby(const vec3& center, float radius, void** out, int max) {
    void* level = Level();
    if (!level || !pFindInRadius) return 0;
    // ttl::vector layout is not public. Give it zeroed room and read it back
    // as either {begin, end, cap} or {data, size, cap}.
    alignas(16) uint64_t vec[8] = {};
    int count = 0;
    __try {
        const void* filter = pControlObjectClass ? pControlObjectClass() : nullptr;
        if (!pFindInRadius(level, vec, &center, radius, filter, false, &count)) return 0;
        void** data = reinterpret_cast<void**>(vec[0]);
        size_t n = 0;
        if (vec[1] > vec[0] && (vec[1] - vec[0]) % 8 == 0 && (vec[1] - vec[0]) / 8 < 100000)
            n = (vec[1] - vec[0]) / 8;
        else if ((vec[1] & 0xffffffff) < 100000)
            n = static_cast<size_t>(vec[1] & 0xffffffff);
        if (!data || !Readable(data, n * 8)) return 0;
        int w = 0;
        for (size_t i = 0; i < n && w < max; ++i)
            if (data[i]) out[w++] = data[i];
        // The engine's allocator owns `data`; we leak it rather than free it with the wrong heap.
        return w;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGW("FindObjectsInRadius faulted (vec %llx %llx %llx)", vec[0], vec[1], vec[2]);
        return 0;
    }
}

void* Parent(void* obj) {
    if (!pGetParent || !Readable(obj, 8)) return nullptr;
    __try {
        return pGetParent(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void DumpNearby(float radius) {
    Camera cam{};
    if (!ReadCamera(cam)) { LOGW("dump: no camera yet"); return; }
    static void* objs[2048];
    int n = FindNearby({cam.pos.x, cam.pos.y, cam.pos.z}, radius, objs, 2048);
    LOGI("dump: %d objects within %.1fm of (%.2f, %.2f, %.2f), fov %.2f aspect %.3f",
         n, radius, cam.pos.x, cam.pos.y, cam.pos.z, cam.fovDeg, cam.aspect);
    for (int i = 0; i < n; ++i) {
        mtx34 m{};
        bool hasX = GetWorldXform(objs[i], m);
        void* parent = Parent(objs[i]);
        LOGI("  %p %-40s pos (%.2f, %.2f, %.2f)%s parent %p %s", objs[i], ClassName(objs[i]),
             m.m[0][3], m.m[1][3], m.m[2][3], hasX ? "" : " [no xform]", parent, parent ? ClassName(parent) : "");
    }
}

// ---------- RTTI casts ----------
// MSVC x64 RTTI: vtable[-1] is the CompleteObjectLocator (offset of this vtable
// inside the complete object), whose ClassHierarchyDescriptor lists every base
// class with its offset. That's enough to cast between any two bases by name.

void* CastTo(void* obj, const char* typeName) {
    __try {
        if (!obj || (reinterpret_cast<uintptr_t>(obj) & 7)) return nullptr;
        void** vtbl = *reinterpret_cast<void***>(obj);
        if (!InGameModule(vtbl - 1)) return nullptr;
        auto* col = reinterpret_cast<const uint32_t*>(vtbl[-1]);
        if (!InGameModule(col) || col[0] != 1) return nullptr;
        uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];
        uint8_t* complete = static_cast<uint8_t*>(obj) - col[1];  // COL.offset
        auto* chd = reinterpret_cast<const uint32_t*>(base + col[4]);
        uint32_t count = chd[2];
        auto* bca = reinterpret_cast<const uint32_t*>(base + chd[3]);
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            auto* bcd = reinterpret_cast<const int32_t*>(base + bca[i]);
            const char* name = reinterpret_cast<const char*>(base + uint32_t(bcd[0]) + 16);
            if (strcmp(name, typeName) != 0) continue;
            int32_t mdisp = bcd[2], pdisp = bcd[3], vdisp = bcd[4];
            uint8_t* p = complete;
            if (pdisp >= 0) {  // virtual base
                auto* vbtable = *reinterpret_cast<int32_t**>(complete + pdisp);
                p += pdisp + vbtable[vdisp / 4];
            }
            return p + mdisp;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------- children ----------

int Children(void* obj, void** out, int max) {
    if (!pGetChildren || !Readable(obj, 8)) return 0;
    alignas(16) uint64_t vec[8] = {};
    __try {
        pGetChildren(obj, vec);
        void** data = reinterpret_cast<void**>(vec[0]);
        size_t n = 0;
        if (vec[1] > vec[0] && (vec[1] - vec[0]) % 8 == 0 && (vec[1] - vec[0]) / 8 < 10000)
            n = (vec[1] - vec[0]) / 8;
        else if ((vec[1] & 0xffffffff) < 10000)
            n = static_cast<size_t>(vec[1] & 0xffffffff);
        if (!data || !Readable(data, n * 8)) return 0;
        int w = 0;
        for (size_t i = 0; i < n && w < max; ++i)
            if (data[i]) out[w++] = data[i];
        return w;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---------- damage ----------
// SDamageInfoDi, built the way gamedll builds it for explosions on destroyable
// objects (gamedll 0x2772d1). 0xA8 bytes; the engine's SDamageInfo is the first
// 0x40: vftable, attacker, victim, amount (0x18), hit position (0x20), direction
// (0x2c), -1, damage type (0x3c). The rest are the game's defaults from there.
// (That code passes 7, COLLISION_FROM_GAME_SCRIPT, which zombies ignore.)

// Where a call into the game faulted, as module+offset, so it can be looked up
// in the disassembly. A few per run.
int LogFault(EXCEPTION_POINTERS* ep, const char* what, void* obj) {
    static int count = 0;
    if (count++ < 8 && ep && ep->ExceptionRecord && ep->ContextRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const CONTEXT* c = ep->ContextRecord;
        auto where = [](uint64_t addr, char* out, size_t n) {
            HMODULE m = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(addr), &m) && m) {
                GetModuleFileNameA(m, name, MAX_PATH);
                const char* base = strrchr(name, '\\');
                sprintf_s(out, n, "%s+0x%llx", base ? base + 1 : name, addr - reinterpret_cast<uint64_t>(m));
            } else {
                sprintf_s(out, n, "%llx", addr);
            }
        };
        char at[MAX_PATH + 32];
        where(reinterpret_cast<uint64_t>(er->ExceptionAddress), at, sizeof(at));
        LOGW("%s on %p faulted: code %08lx at %s, %s %llx | rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx",
             what, obj, er->ExceptionCode, at,
             er->NumberParameters >= 2 ? (er->ExceptionInformation[0] ? "writing" : "reading") : "?",
             er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0, c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi,
             c->Rdi, c->R8, c->R9);
        // The return addresses on the stack show how the game got there.
        const uint64_t* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
        std::string trail;
        for (int i = 0, found = 0; i < 64 && found < 6 && Readable(sp + i, 8); ++i) {
            char w[MAX_PATH + 32];
            where(sp[i], w, sizeof(w));
            if (strstr(w, "gamedll") || strstr(w, "engine_x64")) { trail += ' '; trail += w; ++found; }
        }
        LOGW("  stack:%s", trail.c_str());
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// The pointer the engine uses for an object in SDamageInfo's attacker/victim
// fields: engine 0x13e373 sets victim = [[IControlObject + 8] + 0xa0].
void* DamageIdentity(void* obj) {
    void* ctrl = CastTo(obj, ".?AVIControlObject@@");
    if (!ctrl || !Readable(static_cast<uint8_t*>(ctrl) + 8, 8)) return nullptr;
    __try {
        uint8_t* impl = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctrl) + 8);
        if (!impl || !Readable(impl + 0xa0, 8)) return nullptr;
        return *reinterpret_cast<void**>(impl + 0xa0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool TakeDamage(void* obj, void* attacker, float amount, const vec3& pos, const vec3& dir, uint32_t dlType,
                float impulse, int* boneOut) {
    if (!gGameDll || !Readable(obj, 8)) return false;
    // The hit body part: weapon hits carry the mesh element they struck (gamedll
    // 0x6e8680 picks one with GetMeshElemFromBoneID); explosions pass -1.
    vec3 bonePos = pos;
    int bone = dlType == 3 ? -1 : NearestBone(obj, pos, &bonePos);
    if (boneOut) *boneOut = bone;
    alignas(16) uint8_t buf[0xB0] = {};
    auto put32 = [&](size_t off, uint32_t v) { memcpy(buf + off, &v, 4); };
    auto putf = [&](size_t off, float v) { memcpy(buf + off, &v, 4); };
    auto put64 = [&](size_t off, uint64_t v) { memcpy(buf + off, &v, 8); };
    __try {
        put64(0x00, gGameDll + 0x1443f38);                 // SDamageInfoDi vftable
        put64(0x08, reinterpret_cast<uint64_t>(attacker));  // attacker's IGSObject (complete + 0x28)
        put64(0x10, 0);                                     // victim: TakeDamage fills it in
        putf(0x18, amount);
        putf(0x1c, impulse);                                // weapon hits: max(dist * 30, 135)
        putf(0x20, bonePos.x); putf(0x24, bonePos.y); putf(0x28, bonePos.z);
        putf(0x2c, dir.x); putf(0x30, dir.y); putf(0x34, dir.z);
        put32(0x38, uint32_t(bone));                        // mesh element hit, -1 for none
        put32(0x3c, dlType);                                // EDamageType
        buf[0x40] = 1;
        put32(0x48, *reinterpret_cast<const uint32_t*>(gGameDll + 0x1BA818C));
        if (dlType == 3) {
            // Explosion-style (gamedll 0x2772d1).
            put32(0x54, 0x8000000);
            memcpy(buf + 0x70, reinterpret_cast<const void*>(gGameDll + 0x1927F20), 16);  // -1, 0, -1, 0
        } else {
            // Weapon-hit style (gamedll 0x6e8680).
            put32(0x54, 0x10);
            put32(0x70, 0xffffffff); put32(0x74, 2); put32(0x78, 0xffffffff); put32(0x7c, 0);
        }
        put32(0x80, 0xffffffff);
        putf(0x84, 1.0f);
        putf(0x88, 1.0f);
        put64(0x90, 0);
        putf(0x9c, 1.0f);
        putf(0xa0, -1.0f);
        // IControlObject::TakeDamage is slot 20 of the IControlObject vtable
        // (actor + 0x18). Handles from FindObjectsInRadius already are
        // IControlObject pointers; the cast makes sure, since slot 20 of the
        // actor's primary vtable (+0) is an IModelObject skin setter instead.
        // Engine 0x13e310 clones the info (its vtable slot 1), sets the victim
        // to [[ctrl+8]+0xa0] and hands it to that object's vtable slot 17.
        void* ctrl = CastTo(obj, ".?AVIControlObject@@");
        if (!ctrl || !Readable(ctrl, 8)) return false;
        using TakeDamage_t = void (*)(void* self, const void* info);
        auto fn = reinterpret_cast<TakeDamage_t>((*reinterpret_cast<void***>(ctrl))[20]);
        static bool checked = false;
        if (!checked) {
            checked = true;
            // The vtable slot is a gamedll thunk; the import it jumps to is the engine export.
            static void* exported = reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleA("engine_x64_rwdi.dll"), "?TakeDamage@IControlObject@@UEAAXAEBUSDamageInfo@@@Z"));
            const uint8_t* t = reinterpret_cast<const uint8_t*>(fn);
            void* target = nullptr;
            if (Readable(t, 6) && t[0] == 0xFF && t[1] == 0x25) {
                int32_t disp;
                memcpy(&disp, t + 2, 4);
                void* const* slot = reinterpret_cast<void* const*>(t + 6 + disp);
                if (Readable(slot, 8)) target = *slot;
            }
            LOGI("TakeDamage: IControlObject slot 20 %p -> %p, engine export %p%s", fn, target, exported,
                 target == exported ? " (match)" : " (MISMATCH)");
        }
        fn(ctrl, buf);
        return true;
    } __except (LogFault(GetExceptionInformation(), "TakeDamage", obj)) {
        return false;
    }
}

// ---------- script methods ----------
// Every game object is also a CRTTIObject (at +0x28 in actors), and the game
// registers its script methods by name. No-argument ones are CRTTIVoidMethods.

namespace {
using FindMethod_t = const void* (*)(const void* rttiObj, const char* name);
using CallVoid_t = void (*)(const void* method, void* rttiObj);
FindMethod_t pFindMethod = nullptr;
CallVoid_t pCallVoid = nullptr;
const void* gVoidMethodVtbl = nullptr;
bool gScriptResolved = false;

void ResolveScript() {
    if (gScriptResolved) return;
    gScriptResolved = true;
    HMODULE e = GetModuleHandleA("engine_x64_rwdi.dll");
    pFindMethod = reinterpret_cast<FindMethod_t>(GetProcAddress(e, "?FindMethod@CRTTIObject@@QEBAPEBVCRTTIMethod@@PEBD@Z"));
    pCallVoid = reinterpret_cast<CallVoid_t>(GetProcAddress(e, "?CallVoid@CRTTIVoidMethod@@UEBAXPEAVCRTTIObject@@@Z"));
    gVoidMethodVtbl = GetProcAddress(e, "??_7CRTTIVoidMethod@@6B@");
    LOGI("script methods: FindMethod %p CallVoid %p vtbl %p", pFindMethod, pCallVoid, gVoidMethodVtbl);
}
}  // namespace

bool HasScriptMethod(void* obj, const char* name) {
    ResolveScript();
    void* rtti = CastTo(obj, ".?AVCRTTIObject@@");
    if (!rtti || !pFindMethod) return false;
    __try {
        return pFindMethod(rtti, name) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallScriptMethod(void* obj, const char* name) {
    ResolveScript();
    void* rtti = CastTo(obj, ".?AVCRTTIObject@@");
    if (!rtti || !pFindMethod || !pCallVoid) return false;
    __try {
        const void* m = pFindMethod(rtti, name);
        if (!m) return false;
        // Only no-argument methods; anything else would read missing arguments.
        if (gVoidMethodVtbl && *reinterpret_cast<const void* const*>(m) != gVoidMethodVtbl) {
            LOGW("script method %s isn't a void method; not calling it", name);
            return false;
        }
        pCallVoid(m, rtti);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGW("script method %s on %p faulted", name, obj);
        return false;
    }
}

// ---------- mesh elements (bones) ----------

namespace {
using ElemCount_t = int (*)(const void* model);
using ElemPos_t = vec3* (*)(const void* model, vec3* ret, int elem);
using ElemName_t = const char* (*)(const void* model, int elem);
using ElemIsBone_t = bool (*)(void* model, int elem);
ElemCount_t pElemCount = nullptr;
ElemPos_t pElemPos = nullptr;
ElemName_t pElemName = nullptr;
ElemIsBone_t pElemIsBone = nullptr;
bool gElemResolved = false;

void ResolveElements() {
    if (gElemResolved) return;
    gElemResolved = true;
    HMODULE e = GetModuleHandleA("engine_x64_rwdi.dll");
    pElemCount = reinterpret_cast<ElemCount_t>(GetProcAddress(e, "?GetElementsNumber@IModelObject@@QEBAHXZ"));
    pElemPos = reinterpret_cast<ElemPos_t>(GetProcAddress(e, "?GetElementWorldPos@IModelObject@@QEBA?AVvec3@@H@Z"));
    pElemName = reinterpret_cast<ElemName_t>(GetProcAddress(e, "?GetElementNameCStr@IModelObject@@QEBAPEBDH@Z"));
    pElemIsBone = reinterpret_cast<ElemIsBone_t>(GetProcAddress(e, "?IsElementABone@IModelObject@@QEAA_NH@Z"));
}
}  // namespace

int NearestBone(void* obj, const vec3& p, vec3* bonePos) {
    ResolveElements();
    void* model = CastTo(obj, ".?AVIModelObject@@");
    if (!model || !pElemCount || !pElemPos) return -1;
    __try {
        int n = pElemCount(model), best = -1;
        float bestD = 1e30f;
        for (int i = 0; i < n && i < 512; ++i) {
            if (pElemIsBone && !pElemIsBone(model, i)) continue;
            vec3 q;
            pElemPos(model, &q, i);
            float d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z);
            if (d < bestD) { bestD = d; best = i; if (bonePos) *bonePos = q; }
        }
        return best;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Head and feet from the skeleton ("head", "l_foot", "r_foot" on zombies and
// humans). Element indices are looked up by name once per object.
namespace {
struct PoseIdx { int head, lfoot, rfoot, count; };

int ElemCountSafe(void* model) {
    __try { return pElemCount(model); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool FindPoseIdx(void* model, int n, PoseIdx& idx) {
    idx = {-1, -1, -1, n};
    __try {
        for (int i = 0; i < n && i < 1024; ++i) {
            const char* name = pElemName(model, i);
            if (!name) continue;
            if (!strcmp(name, "head")) idx.head = i;
            else if (!strcmp(name, "l_foot")) idx.lfoot = i;
            else if (!strcmp(name, "r_foot")) idx.rfoot = i;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPose(void* model, const PoseIdx& idx, vec3& head, vec3& feet) {
    if (idx.head < 0 || idx.lfoot < 0 || idx.rfoot < 0) return false;
    __try {
        vec3 l, r;
        pElemPos(model, &head, idx.head);
        pElemPos(model, &l, idx.lfoot);
        pElemPos(model, &r, idx.rfoot);
        feet = {(l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f, (l.z + r.z) * 0.5f};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool GetPose(void* obj, vec3& head, vec3& feet) {
    ResolveElements();
    void* model = CastTo(obj, ".?AVIModelObject@@");
    if (!model || !pElemCount || !pElemPos || !pElemName) return false;
    static std::unordered_map<void*, PoseIdx> cache;
    int n = ElemCountSafe(model);
    if (n <= 0) return false;
    auto it = cache.find(model);
    if (it == cache.end() || it->second.count != n) {
        if (cache.size() > 1024) cache.clear();
        PoseIdx idx;
        if (!FindPoseIdx(model, n, idx)) return false;
        it = cache.insert_or_assign(model, idx).first;
    }
    return ReadPose(model, it->second, head, feet);
}

void LogSkeleton(void* obj) {
    ResolveElements();
    void* model = CastTo(obj, ".?AVIModelObject@@");
    if (!model || !pElemCount || !pElemName || !pElemPos) return;
    __try {
        int n = pElemCount(model);
        LOGI("skeleton of %s %p: %d elements", ClassName(obj), obj, n);
        for (int i = 0; i < n && i < 512; ++i) {
            vec3 q;
            pElemPos(model, &q, i);
            const char* name = pElemName(model, i);
            LOGI("  %3d %-32s bone %d at (%.2f, %.2f, %.2f)", i, name ? name : "?",
                 pElemIsBone ? int(pElemIsBone(model, i)) : -1, q.x, q.y, q.z);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGW("skeleton dump faulted");
    }
}

// ---------- raytrace ----------
// IGSObject::Raytrace(flags, SCollision*, from, to&, ushort, ignore, 0, 0, 0)
// reaches the level's physics world through any game object and moves `to` to
// the hit point. SCollision (0x40): 6 floats (point, normal?), then pointers at
// 0x18, 0x28, 0x30 (one of them the object hit), and two words at 0x38.

namespace {
using Raytrace_t = uint8_t (*)(void* gs, uint8_t flags, void* coll, const vec3* from, vec3* to, uint16_t mask,
                               void* ignore, uint32_t a, uint32_t b, int c);
Raytrace_t pRaytrace = nullptr;
}  // namespace

bool Raytrace(void* fromObj, const vec3& from, const vec3& to, RayHit& out, void* ignore) {
    if (!pRaytrace)
        pRaytrace = reinterpret_cast<Raytrace_t>(GetProcAddress(GetModuleHandleA("engine_x64_rwdi.dll"),
            "?Raytrace@IGSObject@@QEAAEEPEAUSCollision@@AEBVvec3@@AEAV3@GPEAVIControlObject@@IIH@Z"));
    void* gs = fromObj ? CastTo(fromObj, ".?AVIGSObject@@") : nullptr;
    if (!pRaytrace || !gs) return false;
    alignas(16) uint8_t coll[0x40] = {};
    memset(coll + 0x20, 0xff, 4);
    vec3 end = to;
    __try {
        bool hit = pRaytrace(gs, 0x37, coll, &from, &end, 0, ignore, 0, 0, 0) != 0;
        out.hit = hit;
        out.pos = end;
        memcpy(out.floats, coll, sizeof(out.floats));
        memcpy(&out.p18, coll + 0x18, 8);
        memcpy(&out.p28, coll + 0x28, 8);
        memcpy(&out.p30, coll + 0x30, 8);
        float dx = end.x - from.x, dy = end.y - from.y, dz = end.z - from.z;
        out.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGW("raytrace faulted");
        return false;
    }
}

// ---------- pinning ----------
// While GMod holds or throws an actor, its AI keeps moving it every frame; the
// two fight and the zombie jitters. Instead, every engine call that sets a
// pinned object's transform gets the position GMod wants.

// The public setters all go through the object's engine record ([ctrl+8]) and its
// transform node ([record+0xe8]). Positions live in a transform table the node
// indexes. The game's AI also moves actors with SetWorldPosition / SetLocalPosition
// / SetWorldPosDir, which write that table directly; those weren't pinned, so a
// held zombie snapped back to where its AI thought it was. Now the internal
// setters are hooked: node-level matrix writers (engine 0x1329c0 world, 0x132a50
// world without propagation, 0x132910 local) and record-level position writers
// (0x136220 world, 0x136030 local), plus the SetWorldPosDir export.

namespace {
struct Pin { void* obj; void* rec; void* node; vec3 pos; };
constexpr int kMaxPins = 16;
Pin gPins[kMaxPins];
std::atomic<int> gPinCount{0};  // lock-free: read on the engine's hottest path

using NodeXform_t = void (*)(void* node, const mtx34* m);
using RecPos_t = void (*)(void* rec, const vec3* p);
using PosDir_t = void (*)(void* ctrl, const vec3* p, const vec3* d);
NodeXform_t oNodeWorld = nullptr, oNodeWorldNP = nullptr, oNodeLocal = nullptr;
RecPos_t oRecWorldPos = nullptr, oRecLocalPos = nullptr;
PosDir_t oSetWorldPosDir = nullptr;

// Where the game itself last tried to put each held actor (its AI's own idea).
std::mutex gGamePosMutex;
std::unordered_map<void*, vec3> gGamePos;
vec3 Translation(const mtx34* m) { return {m->m[0][3], m->m[1][3], m->m[2][3]}; }

// Which threads write held actors' positions (the game's AI and physics), and
// through which setter: tells where game logic runs. Logged once per pair.
void NotePinWrite(const char* via) {
    static std::atomic<int> logged{0};
    static std::mutex m;
    static std::vector<std::pair<DWORD, const char*>> seen;
    if (logged.load(std::memory_order_relaxed) >= 12) return;
    std::lock_guard<std::mutex> g(m);
    DWORD t = GetCurrentThreadId();
    for (auto& s : seen)
        if (s.first == t && s.second == via) return;
    seen.push_back({t, via});
    logged++;
    LOGI("held actor moved by thread %lu via %s", t, via);
}

template <class F>
bool FindPin(F match, vec3& out, const char* via, vec3 incoming) {
    int n = gPinCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (match(gPins[i])) {
            out = gPins[i].pos;
            NotePinWrite(via);
            // Not our own write: it's where the game thinks the actor is.
            float dx = incoming.x - out.x, dy = incoming.y - out.y, dz = incoming.z - out.z;
            if (dx * dx + dy * dy + dz * dz > 0.0001f) {
                std::lock_guard<std::mutex> g(gGamePosMutex);
                gGamePos[gPins[i].obj] = incoming;
            }
            return true;
        }
    return false;
}

const mtx34* Pinned(const mtx34* m, vec3 p, mtx34& tmp) {
    tmp = *m;
    tmp.m[0][3] = p.x;
    tmp.m[1][3] = p.y;
    tmp.m[2][3] = p.z;
    return &tmp;
}

// Actors have no parent, so local == world for everything pinned.
void HkNodeWorld(void* node, const mtx34* m) {
    vec3 p; mtx34 t;
    if (m && FindPin([&](const Pin& q) { return q.node == node; }, p, "node world xform", Translation(m))) m = Pinned(m, p, t);
    oNodeWorld(node, m);
}
void HkNodeWorldNP(void* node, const mtx34* m) {
    vec3 p; mtx34 t;
    if (m && FindPin([&](const Pin& q) { return q.node == node; }, p, "node world xform np", Translation(m))) m = Pinned(m, p, t);
    oNodeWorldNP(node, m);
}
void HkNodeLocal(void* node, const mtx34* m) {
    vec3 p; mtx34 t;
    if (m && FindPin([&](const Pin& q) { return q.node == node; }, p, "node local xform", Translation(m))) m = Pinned(m, p, t);
    oNodeLocal(node, m);
}
void HkRecWorldPos(void* rec, const vec3* v) {
    vec3 p;
    if (v && FindPin([&](const Pin& q) { return q.rec == rec; }, p, "world position", *v)) v = &p;
    oRecWorldPos(rec, v);
}
void HkRecLocalPos(void* rec, const vec3* v) {
    vec3 p;
    if (v && FindPin([&](const Pin& q) { return q.rec == rec; }, p, "local position", *v)) v = &p;
    oRecLocalPos(rec, v);
}
void HkSetWorldPosDir(void* ctrl, const vec3* v, const vec3* d) {
    vec3 p;
    if (v && FindPin([&](const Pin& q) { return q.obj == ctrl; }, p, "SetWorldPosDir", *v)) v = &p;
    oSetWorldPosDir(ctrl, v, d);
}

// Hook an engine-internal function only if its first bytes are what this build had.
bool HookAt(uintptr_t base, uintptr_t rva, const uint8_t (&expect)[16], void* hook, void** orig, const char* what) {
    void* at = reinterpret_cast<void*>(base + rva);
    if (!Readable(at, 16) || memcmp(at, expect, 16) != 0) {
        LOGW("pin hook %s: engine+0x%llx isn't the expected code (different game version?); skipped", what,
             static_cast<unsigned long long>(rva));
        return false;
    }
    bool ok = MH_CreateHook(at, hook, orig) == MH_OK && MH_EnableHook(at) == MH_OK;
    if (!ok) LOGW("pin hook %s failed", what);
    return ok;
}
}  // namespace

bool InstallPinHooks() {
    HMODULE e = GetModuleHandleA("engine_x64_rwdi.dll");
    uintptr_t base = reinterpret_cast<uintptr_t>(e);
    static const uint8_t kNodeWorld[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xd9};
    static const uint8_t kNodeWorldNP[16] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48};
    static const uint8_t kNodeLocal[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xd9};
    static const uint8_t kRecPos[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x57, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00, 0x0f};
    int n = 0;
    n += HookAt(base, 0x1329c0, kNodeWorld, &HkNodeWorld, reinterpret_cast<void**>(&oNodeWorld), "node world xform");
    n += HookAt(base, 0x132a50, kNodeWorldNP, &HkNodeWorldNP, reinterpret_cast<void**>(&oNodeWorldNP), "node world xform np");
    n += HookAt(base, 0x132910, kNodeLocal, &HkNodeLocal, reinterpret_cast<void**>(&oNodeLocal), "node local xform");
    n += HookAt(base, 0x136220, kRecPos, &HkRecWorldPos, reinterpret_cast<void**>(&oRecWorldPos), "world position");
    n += HookAt(base, 0x136030, kRecPos, &HkRecLocalPos, reinterpret_cast<void**>(&oRecLocalPos), "local position");
    if (void* pd = GetProcAddress(e, "?SetWorldPosDir@IControlObject@@QEAAXAEBVvec3@@0@Z"))
        n += MH_CreateHook(pd, &HkSetWorldPosDir, reinterpret_cast<void**>(&oSetWorldPosDir)) == MH_OK &&
             MH_EnableHook(pd) == MH_OK;
    LOGI("pin hooks: %d of 6 installed", n);
    return n > 0;
}

void SetPins(void* const* objs, const vec3* pos, int n) {
    n = std::min(n, kMaxPins);
    // Readers don't lock: hide the list while it's rewritten, then publish it.
    gPinCount.store(0, std::memory_order_release);
    int w = 0;
    for (int i = 0; i < n; ++i) {
        void* rec = Readable(static_cast<uint8_t*>(objs[i]) + 8, 8) ? *reinterpret_cast<void**>(static_cast<uint8_t*>(objs[i]) + 8) : nullptr;
        void* node = rec && Readable(static_cast<uint8_t*>(rec) + 0xe8, 8) ? *reinterpret_cast<void**>(static_cast<uint8_t*>(rec) + 0xe8) : nullptr;
        if (!rec || !node) continue;
        gPins[w++] = {objs[i], rec, node, pos[i]};
    }
    gPinCount.store(w, std::memory_order_release);
}

// ---------- game objects ----------
// Game-side objects (IGSObject: PlayerFppVis, WeaponVis, ...) aren't all engine
// objects. Each has an engine record at +0x20 (engine IGSObject::GetNext 0x251ec0);
// the record's +0x30 is the next record in the level's list, its +0x20 the game
// object, and its +0xa0 the IControlObject (the model) when there is one. The
// same record sits at IControlObject+8, which is how TakeDamage finds its victim.

namespace {
using GetObjectByID_t = void* (*)(const void* level, int id);
GetObjectByID_t pGetObjectByID = nullptr;

void* RecordOf(void* gso) {
    if (!Readable(static_cast<uint8_t*>(gso) + 0x20, 8)) return nullptr;
    void* rec = *reinterpret_cast<void**>(static_cast<uint8_t*>(gso) + 0x20);
    return rec && Readable(rec, 0xa8) ? rec : nullptr;
}

void* ObjectByIdSafe(void* level, int id) {
    __try { return pGetObjectByID(level, id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The list from `rec` onward; at most `max` objects.
int WalkFrom(void* rec, void** out, int n, int max) {
    __try {
        for (int guard = 0; rec && n < max && guard < 200000; ++guard) {
            void* gso = *reinterpret_cast<void**>(static_cast<uint8_t*>(rec) + 0x20);
            if (gso) out[n++] = gso;
            void* next = *reinterpret_cast<void**>(static_cast<uint8_t*>(rec) + 0x30);
            if (!next || next == rec) break;  // a bad pointer faults into __except
            rec = next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
}  // namespace

int AllGameObjects(void* anyGso, void** out, int max) {
    // The list's start: the lowest id that exists (ids count up from 0 or 1).
    void* start = nullptr;
    void* level = gLevel.load(std::memory_order_relaxed);
    if (!pGetObjectByID)
        pGetObjectByID = reinterpret_cast<GetObjectByID_t>(
            GetProcAddress(GetModuleHandleA("engine_x64_rwdi.dll"), "?GetObjectByID@ILevel@@QEBAPEAVIGSObject@@H@Z"));
    if (level && pGetObjectByID)
        for (int id = 0; id < 256 && !start; ++id)
            if (void* gso = ObjectByIdSafe(level, id)) start = RecordOf(gso);
    int n = start ? WalkFrom(start, out, 0, max) : 0;
    // And from a known object, in case the list isn't in id order.
    bool seen = false;
    for (int i = 0; i < n && !seen; ++i) seen = out[i] == anyGso;
    if (!seen && anyGso)
        if (void* rec = RecordOf(anyGso)) n = WalkFrom(rec, out, n, max);
    return n;
}

int ObjectsReferencedBy(void* obj, size_t bytes, void** out, uint32_t* offsets, int max) {
    int n = 0;
    auto* p = static_cast<uint8_t*>(obj);
    if (!obj || (reinterpret_cast<uintptr_t>(obj) & 7)) return 0;
    for (size_t off = 0; off + 8 <= bytes && n < max; off += 8) {
        uintptr_t v = 0;
        __try {
            v = *reinterpret_cast<uintptr_t*>(p + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;  // ran off the end of the allocation's pages
        }
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFull || (v & 7) || v == reinterpret_cast<uintptr_t>(obj)) continue;
        if (ClassName(reinterpret_cast<void*>(v))[0] != '.') continue;  // not a polymorphic object
        out[n] = reinterpret_cast<void*>(v);
        if (offsets) offsets[n] = uint32_t(off);
        ++n;
    }
    return n;
}

bool TakeGamePosition(void* obj, vec3& out) {
    std::lock_guard<std::mutex> g(gGamePosMutex);
    auto it = gGamePos.find(obj);
    if (it == gGamePos.end()) return false;
    out = it->second;
    gGamePos.erase(it);
    return true;
}

namespace {
void* CompleteObject(void* obj) {
    __try {
        void** vtbl = *reinterpret_cast<void***>(obj);
        if (!InGameModule(vtbl - 1)) return nullptr;
        auto* col = reinterpret_cast<const uint32_t*>(vtbl[-1]);
        if (!InGameModule(col) || col[0] != 1) return nullptr;
        return static_cast<uint8_t*>(obj) - col[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Every 3 consecutive floats in [p, p+bytes) equal to `from` (to the millimetre)
// become `to`. Offsets of the first few are reported.
int ReplaceIn(void* p, size_t bytes, const vec3& from, const vec3& to, uint32_t* offs, int& nOffs) {
    int n = 0;
    auto* f = static_cast<float*>(p);
    for (size_t i = 0; i + 3 <= bytes / 4; ++i) {
        __try {
            if (std::fabs(f[i] - from.x) < 0.001f && std::fabs(f[i + 1] - from.y) < 0.001f &&
                std::fabs(f[i + 2] - from.z) < 0.001f) {
                f[i] = to.x; f[i + 1] = to.y; f[i + 2] = to.z;
                if (nOffs < 8) offs[nOffs++] = uint32_t(i * 4);
                ++n;
                i += 2;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return n;
}
}  // namespace

int MoveGameCopies(void* obj, const vec3& from, const vec3& to) {
    void* complete = CompleteObject(obj);
    if (!complete) return 0;
    uint32_t offs[8];
    int nOffs = 0;
    int n = ReplaceIn(complete, 0x3000, from, to, offs, nOffs);
    std::string where;
    for (int i = 0; i < nOffs; ++i) where += " +0x" + [&] { char b[16]; sprintf_s(b, "%x", offs[i]); return std::string(b); }();
    // And in the objects it points to (its movement controller, physics proxy, ...).
    void* refs[256];
    uint32_t at[256];
    int nr = ObjectsReferencedBy(complete, 0x3000, refs, at, 256);
    for (int r = 0; r < nr; ++r) {
        nOffs = 0;
        int k = ReplaceIn(refs[r], 0x800, from, to, offs, nOffs);
        if (k) {
            char b[160];
            sprintf_s(b, " | %s (at +0x%x) x%d", ClassName(refs[r]), at[r], k);
            where += b;
        }
        n += k;
    }
    LOGI("release: moved %d copies of the game's position for %s %p:%s", n, ClassName(obj), obj, where.c_str());
    return n;
}

void* ModelOf(void* gso) {
    void* rec = RecordOf(gso);
    if (!rec) return nullptr;
    void* ctrl = *reinterpret_cast<void**>(static_cast<uint8_t*>(rec) + 0xa0);
    if (!ctrl || !Readable(ctrl, 16)) return nullptr;
    // Must really be an IControlObject whose record is this one.
    if (CastTo(ctrl, ".?AVIControlObject@@") != ctrl) return nullptr;
    if (*reinterpret_cast<void**>(static_cast<uint8_t*>(ctrl) + 8) != rec) return nullptr;
    return ctrl;
}

// ---------- game tick ----------
// ILevel::TimerUpdate is called once per game update from gamedll's main update
// (gamedll 0x12192cf). Work that touches game objects belongs there, on the game's
// own thread, rather than in Present, which may run on the render thread.

namespace {
using TimerUpdate_t = void (*)(void* level);
TimerUpdate_t oTimerUpdate = nullptr;
TickCallback gTickCb = nullptr;

void HkTimerUpdate(void* level) {
    oTimerUpdate(level);
    static DWORD logged = 0;
    if (!logged) {
        logged = GetCurrentThreadId();
        LOGI("game tick: first ILevel::TimerUpdate on thread %lu", logged);
    }
    if (gTickCb) gTickCb();
}
}  // namespace

bool InstallTickHook(TickCallback cb) {
    void* f = GetProcAddress(GetModuleHandleA("engine_x64_rwdi.dll"), "?TimerUpdate@ILevel@@QEAAXXZ");
    gTickCb = cb;
    bool ok = f && MH_CreateHook(f, &HkTimerUpdate, reinterpret_cast<void**>(&oTimerUpdate)) == MH_OK &&
              MH_EnableHook(f) == MH_OK;
    LOGI("game tick hook %s", ok ? "installed" : "FAILED (world work stays in Present)");
    return ok;
}

// ---------- rendering ----------
// The game turns models back on itself (climbing and balancing showed Kyle's arms
// again, round 10), from ~10 places in gamedll. So EnableRendering is hooked:
// while GMod's hands are out, models on the force-hidden list stay hidden.

namespace {
EnableRendering_t oEnableRendering = nullptr;  // the original, once hooked
constexpr int kMaxForceHidden = 64;
void* gForceHidden[kMaxForceHidden];
std::atomic<int> gForceHiddenN{0};
std::atomic<int> gBlockedShows{0};

void HkEnableRendering(void* model, bool on) {
    if (on) {
        int n = gForceHiddenN.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (gForceHidden[i] == model) {
                on = false;
                gBlockedShows.fetch_add(1, std::memory_order_relaxed);
                break;
            }
    }
    oEnableRendering(model, on);
}
}  // namespace

bool InstallRenderHook() {
    if (!pEnableRendering) return false;
    bool ok = MH_CreateHook(reinterpret_cast<void*>(pEnableRendering), &HkEnableRendering,
                            reinterpret_cast<void**>(&oEnableRendering)) == MH_OK &&
              MH_EnableHook(reinterpret_cast<void*>(pEnableRendering)) == MH_OK;
    LOGI("rendering hook %s", ok ? "installed" : "FAILED (the game can show hidden models again)");
    return ok;
}

void SetForceHidden(void* const* objs, int n) {
    gForceHiddenN.store(0, std::memory_order_release);
    int w = 0;
    for (int i = 0; i < n && w < kMaxForceHidden; ++i)
        if (void* model = CastTo(objs[i], ".?AVIModelObject@@")) gForceHidden[w++] = model;
    gForceHiddenN.store(w, std::memory_order_release);
}

int TakeBlockedShows() { return gBlockedShows.exchange(0); }

// ---------- HUD elements ----------
// Dying Light's weapon HUD (HudPrimaryWeaponIndicator, ...) is made of
// IUIElements; the game shows them again on its own, so SetVisible is hooked
// the same way as EnableRendering.

namespace {
using UISetVisible_t = void (*)(void* elem, bool on);
using UIIsVisible_t = bool (*)(const void* elem);
UISetVisible_t pUISetVisible = nullptr, oUISetVisible = nullptr;
UIIsVisible_t pUIIsVisible = nullptr;
void* gForceHiddenUI[kMaxForceHidden];
std::atomic<int> gForceHiddenUIN{0};

void HkUISetVisible(void* elem, bool on) {
    if (on) {
        int n = gForceHiddenUIN.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (gForceHiddenUI[i] == elem) { on = false; break; }
    }
    oUISetVisible(elem, on);
}
}  // namespace

bool InstallUIHook() {
    HMODULE e = GetModuleHandleA("engine_x64_rwdi.dll");
    pUISetVisible = reinterpret_cast<UISetVisible_t>(GetProcAddress(e, "?SetVisible@IUIElement@@QEAAX_N@Z"));
    pUIIsVisible = reinterpret_cast<UIIsVisible_t>(GetProcAddress(e, "?IsVisible@IUIElement@@QEBA_NXZ"));
    bool ok = pUISetVisible && MH_CreateHook(reinterpret_cast<void*>(pUISetVisible), &HkUISetVisible,
                                             reinterpret_cast<void**>(&oUISetVisible)) == MH_OK &&
              MH_EnableHook(reinterpret_cast<void*>(pUISetVisible)) == MH_OK;
    LOGI("HUD hook %s", ok ? "installed" : "FAILED");
    return ok;
}

bool UIVisible(void* elem) {
    void* ui = CastTo(elem, ".?AVIUIElement@@");
    if (!ui || !pUIIsVisible) return false;
    __try { return pUIIsVisible(ui); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetUIVisible(void* elem, bool on) {
    auto fn = oUISetVisible ? oUISetVisible : pUISetVisible;
    void* ui = CastTo(elem, ".?AVIUIElement@@");
    if (!ui || !fn) return false;
    __try { fn(ui, on); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SetForceHiddenUI(void* const* elems, int n) {
    gForceHiddenUIN.store(0, std::memory_order_release);
    int w = 0;
    for (int i = 0; i < n && w < kMaxForceHidden; ++i)
        if (void* ui = CastTo(elems[i], ".?AVIUIElement@@")) gForceHiddenUI[w++] = ui;
    gForceHiddenUIN.store(w, std::memory_order_release);
}

bool SetRendering(void* obj, bool on) {
    auto fn = oEnableRendering ? oEnableRendering : reinterpret_cast<EnableRendering_t>(pEnableRendering);
    if (!fn) return false;
    void* model = CastTo(obj, ".?AVIModelObject@@");
    if (!model) return false;
    __try {
        fn(model, on);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace gml::eng
